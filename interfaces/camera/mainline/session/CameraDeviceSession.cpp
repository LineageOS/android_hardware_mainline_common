/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Session"

#include "session/CameraDeviceSession.h"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>

#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/sync.h>
#include <system/camera_metadata.h>

#include "convert/FormatConverter.h"
#include "device/RequestTemplates.h"
#include "jpeg/JpegOutput.h"
#include "session/RequestSettings.h"
#include "utils/Status.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::common::NativeHandle;
using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using device::BufferStatus;
using device::CaptureRequest;
using device::CaptureResult;
using device::ErrorCode;
using device::ErrorMsg;
using device::NotifyMsg;
using device::ShutterMsg;
using device::StreamBuffer;

// Size of the request and result metadata queues.
constexpr size_t kMetadataQueueSize = 1 << 20;

// Longest time to wait for the consumer to release an output buffer.
constexpr int kAcquireFenceTimeoutMs = 1000;

bool IsEmpty(const NativeHandle& handle) {
    return handle.fds.empty() && handle.ints.empty();
}

// The pixel format buffers of a stream are allocated with.
PixelFormat EffectiveFormat(const device::Stream& stream, bool prefer_rgb) {
    if (stream.format != PixelFormat::IMPLEMENTATION_DEFINED) return stream.format;
    // Video encoders want YUV; everything else may get RGB when preferred,
    // which GPU based consumers handle best.
    const bool encoder = (static_cast<int64_t>(stream.usage) &
                          static_cast<int64_t>(BufferUsage::VIDEO_ENCODER)) != 0;
    return prefer_rgb && !encoder ? PixelFormat::RGBA_8888 : PixelFormat::YCBCR_420_888;
}

}  // namespace

std::shared_ptr<CameraDeviceSession> CameraDeviceSession::Create(
        std::string name, std::shared_ptr<const CameraDescription> description,
        std::shared_ptr<device::ICameraDeviceCallback> callback, const DeviceOpeners& open,
        std::shared_ptr<GraphicBuffers> buffers, Status* status, std::shared_ptr<Flash> flash) {
    auto failed = [&](const ::android::base::ResultError<>& error) {
        LOG(ERROR) << name << ": " << error.message();
        const int code = error.code().value();
        *status = code == ENODEV || code == ENOENT ? Status::CAMERA_DISCONNECTED
                  : code == EBUSY                  ? Status::CAMERA_IN_USE
                                                   : Status::INTERNAL_ERROR;
        return nullptr;
    };

    auto device = open.video(description->candidate().info.path);
    if (!device.ok()) return failed(device.error());
    std::unique_ptr<PipelineController> pipeline;
    if (description->candidate().pipeline != nullptr) {
        auto controller = PipelineController::Open(description->candidate().pipeline, open);
        if (!controller.ok()) return failed(controller.error());
        pipeline = std::move(*controller);
    }

    auto session = ::ndk::SharedRefBase::make<CameraDeviceSession>(
            std::move(name), std::move(description), std::move(callback), std::move(*device),
            std::move(pipeline), std::move(buffers), std::move(flash));
    if (session->request_queue_ == nullptr || session->result_queue_ == nullptr) {
        *status = Status::INTERNAL_ERROR;
        return nullptr;
    }
    session->worker_ = std::thread([raw = session.get()] { raw->Run(); });
    LOG(INFO) << session->name_ << ": session opened";
    *status = Status::OK;
    return session;
}

CameraDeviceSession::CameraDeviceSession(std::string name,
                                         std::shared_ptr<const CameraDescription> description,
                                         std::shared_ptr<device::ICameraDeviceCallback> callback,
                                         std::unique_ptr<VideoDevice> device,
                                         std::unique_ptr<PipelineController> pipeline,
                                         std::shared_ptr<GraphicBuffers> buffers,
                                         std::shared_ptr<Flash> flash)
    : name_(std::move(name)),
      description_(std::move(description)),
      callback_(std::move(callback)),
      buffers_(std::move(buffers)),
      flash_(std::move(flash)),
      capture_(std::move(device), std::move(pipeline)),
      flash_control_(flash_ != nullptr) {
    jpeg_context_.characteristics = &description_->characteristics();
    jpeg_context_.make = ::android::base::GetProperty("ro.product.manufacturer", "");
    // External cameras are their own product.
    jpeg_context_.model = description_->internal()
                                  ? ::android::base::GetProperty("ro.product.model", "")
                                  : description_->candidate().info.card;
    request_queue_ = std::make_unique<MetadataQueue>(kMetadataQueueSize, false);
    if (!request_queue_->isValid()) {
        LOG(ERROR) << name_ << ": invalid request metadata queue";
        request_queue_.reset();
    }
    result_queue_ = std::make_unique<MetadataQueue>(kMetadataQueueSize, false);
    if (!result_queue_->isValid()) {
        LOG(ERROR) << name_ << ": invalid result metadata queue";
        result_queue_.reset();
    }
}

CameraDeviceSession::~CameraDeviceSession() {
    close();
}

bool CameraDeviceSession::IsClosed() {
    std::lock_guard<std::mutex> lock(lock_);
    return closed_;
}

::ndk::ScopedAStatus CameraDeviceSession::close() {
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (closed_) return ::ndk::ScopedAStatus::ok();
        // Pending requests are returned with errors on the way out.
        stopping_ = true;
    }
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(lock_);
    capture_.Stop();
    if (flash_ != nullptr) flash_->Release();
    FreeAllBuffersLocked();
    streams_.clear();
    closed_ = true;
    LOG(INFO) << name_ << ": session closed";
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::configureStreams(
        const device::StreamConfiguration& config, std::vector<device::HalStream>* hal_streams) {
    hal_streams->clear();
    std::string why;
    const auto mode = description_->PlanStreams(config, &why);
    if (!mode.has_value()) {
        LOG(ERROR) << name_ << ": configureStreams: " << why;
        return ToBinderStatus(Status::ILLEGAL_ARGUMENT);
    }

    std::lock_guard<std::mutex> lock(lock_);
    if (closed_ || stopping_) return ToBinderStatus(Status::INTERNAL_ERROR);
    if (!queue_.empty() || busy_) {
        LOG(ERROR) << name_ << ": configureStreams with requests in flight";
        return ToBinderStatus(Status::INTERNAL_ERROR);
    }

    const bool prefer_rgb = description_->candidate().prefer_rgb;
    std::map<int32_t, OutputStream> streams;
    for (const auto& stream : config.streams) {
        const PixelFormat format = EffectiveFormat(stream, prefer_rgb);
        // The framework sizes JPEG buffers itself and says so in bufferSize.
        const int32_t buffer_size = format != PixelFormat::BLOB ? 0
                                    : stream.bufferSize > 0     ? stream.bufferSize
                                                                : description_->jpeg_max_size();
        streams[stream.id] = {{stream.width, stream.height}, format, buffer_size};

        device::HalStream hal;
        hal.id = stream.id;
        hal.overrideFormat = format;
        hal.producerUsage = BufferUsage::CPU_WRITE_OFTEN;
        hal.consumerUsage = static_cast<BufferUsage>(0);
        hal.maxBuffers = kPipelineMaxDepth;
        hal.overrideDataSpace = stream.dataSpace;
        hal.supportOffline = false;
        hal.enableHalBufferManager = false;
        hal_streams->push_back(std::move(hal));
        LOG(INFO) << name_ << ": stream " << stream.id << ": " << stream.width << "x"
                  << stream.height << " " << toString(stream.format) << " -> " << toString(format);
    }

    // Buffers of streams that are gone.
    for (auto it = buffer_cache_.begin(); it != buffer_cache_.end();) {
        if (streams.count(it->first) != 0) {
            ++it;
            continue;
        }
        for (auto& [id, handle] : it->second) buffers_->Free(handle);
        it = buffer_cache_.erase(it);
    }
    streams_ = std::move(streams);
    capture_.Configure(*mode);
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::configureStreamsV2(
        const device::StreamConfiguration& config, device::ConfigureStreamsRet* result) {
    return configureStreams(config, &result->halStreams);
}

::ndk::ScopedAStatus CameraDeviceSession::constructDefaultRequestSettings(
        device::RequestTemplate type, device::CameraMetadata* settings) {
    auto metadata = BuildRequestTemplate(*description_, type);
    if (!metadata.has_value()) {
        *settings = {};
        return ToBinderStatus(Status::ILLEGAL_ARGUMENT);
    }
    *settings = metadata->ToAidl();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::flush() {
    std::unique_lock<std::mutex> lock(lock_);
    if (closed_) return ::ndk::ScopedAStatus::ok();
    flushing_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return queue_.empty() && !busy_; });
    flushing_ = false;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::getCaptureRequestMetadataQueue(
        MQDescriptor<int8_t, SynchronizedReadWrite>* queue) {
    *queue = request_queue_->dupeDesc();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::getCaptureResultMetadataQueue(
        MQDescriptor<int8_t, SynchronizedReadWrite>* queue) {
    *queue = result_queue_->dupeDesc();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::isReconfigurationRequired(
        const device::CameraMetadata& /*old_params*/, const device::CameraMetadata& /*new_params*/,
        bool* required) {
    // There are no session parameters.
    *required = false;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::processCaptureRequest(
        const std::vector<CaptureRequest>& requests, const std::vector<device::BufferCache>& caches,
        int32_t* processed) {
    *processed = 0;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (closed_ || stopping_) return ToBinderStatus(Status::INTERNAL_ERROR);
        FreeBuffersLocked(caches);
    }

    for (const auto& request : requests) {
        PendingRequest pending;
        const Status status = Prepare(request, &pending);
        if (status != Status::OK) {
            LOG(ERROR) << name_ << ": rejected request " << request.frameNumber;
            return ToBinderStatus(status);
        }
        {
            std::lock_guard<std::mutex> lock(lock_);
            queue_.push_back(std::move(pending));
        }
        changed_.notify_all();
        ++*processed;
    }
    return ::ndk::ScopedAStatus::ok();
}

Status CameraDeviceSession::Prepare(const CaptureRequest& request, PendingRequest* pending) {
    pending->frame_number = request.frameNumber;
    if (request.inputBuffer.streamId != -1) {
        LOG(ERROR) << name_ << ": reprocessing is not supported";
        return Status::ILLEGAL_ARGUMENT;
    }
    if (request.outputBuffers.empty()) {
        LOG(ERROR) << name_ << ": request " << request.frameNumber << " without output buffers";
        return Status::ILLEGAL_ARGUMENT;
    }

    // Settings come through the queue or inline; the queue has to be read in
    // request order.
    device::CameraMetadata from_queue;
    const device::CameraMetadata* raw = &request.settings;
    if (request.fmqSettingsSize > 0) {
        from_queue.metadata.resize(static_cast<size_t>(request.fmqSettingsSize));
        if (request_queue_ == nullptr ||
            !request_queue_->read(reinterpret_cast<int8_t*>(from_queue.metadata.data()),
                                  from_queue.metadata.size())) {
            LOG(ERROR) << name_ << ": failed to read settings of request " << request.frameNumber;
            return Status::INTERNAL_ERROR;
        }
        raw = &from_queue;
    }

    std::lock_guard<std::mutex> lock(lock_);
    if (!raw->metadata.empty()) {
        auto settings = Metadata::FromAidl(*raw);
        if (!settings.has_value()) return Status::ILLEGAL_ARGUMENT;
        last_settings_ = std::make_shared<const Metadata>(std::move(*settings));
    } else if (last_settings_ == nullptr) {
        LOG(ERROR) << name_ << ": first request " << request.frameNumber << " without settings";
        return Status::ILLEGAL_ARGUMENT;
    }
    pending->settings = last_settings_;

    for (const StreamBuffer& buffer : request.outputBuffers) {
        auto stream = streams_.find(buffer.streamId);
        if (stream == streams_.end()) {
            LOG(ERROR) << name_ << ": request " << request.frameNumber << ": unknown stream "
                       << buffer.streamId;
            return Status::ILLEGAL_ARGUMENT;
        }

        auto& cache = buffer_cache_[buffer.streamId];
        auto cached = cache.find(buffer.bufferId);
        if (cached == cache.end()) {
            if (IsEmpty(buffer.buffer)) {
                LOG(ERROR) << name_ << ": unknown buffer " << buffer.bufferId << " of stream "
                           << buffer.streamId;
                return Status::ILLEGAL_ARGUMENT;
            }
            buffer_handle_t handle = buffers_->Import(buffer.buffer);
            if (handle == nullptr) return Status::INTERNAL_ERROR;
            cached = cache.emplace(buffer.bufferId, handle).first;
        }

        OutputBuffer output;
        output.stream_id = buffer.streamId;
        output.buffer_id = buffer.bufferId;
        output.stream = stream->second;
        output.handle = cached->second;
        if (!buffer.acquireFence.fds.empty() && buffer.acquireFence.fds[0].get() >= 0) {
            output.acquire_fence.reset(dup(buffer.acquireFence.fds[0].get()));
        }
        pending->outputs.push_back(std::move(output));
    }
    return Status::OK;
}

void CameraDeviceSession::FreeBuffersLocked(const std::vector<device::BufferCache>& caches) {
    for (const auto& entry : caches) {
        auto stream = buffer_cache_.find(entry.streamId);
        if (stream == buffer_cache_.end()) continue;
        auto buffer = stream->second.find(entry.bufferId);
        if (buffer == stream->second.end()) continue;
        buffers_->Free(buffer->second);
        stream->second.erase(buffer);
    }
}

void CameraDeviceSession::FreeAllBuffersLocked() {
    for (auto& [stream, cache] : buffer_cache_) {
        for (auto& [id, handle] : cache) buffers_->Free(handle);
    }
    buffer_cache_.clear();
}

::ndk::ScopedAStatus CameraDeviceSession::signalStreamFlush(
        const std::vector<int32_t>& /*stream_ids*/, int32_t /*stream_config_counter*/) {
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDeviceSession::switchToOffline(
        const std::vector<int32_t>& /*streams*/, device::CameraOfflineSessionInfo* /*info*/,
        std::shared_ptr<device::ICameraOfflineSession>* session) {
    *session = nullptr;
    return ToBinderStatus(Status::OPERATION_NOT_SUPPORTED);
}

::ndk::ScopedAStatus CameraDeviceSession::repeatingRequestEnd(
        int32_t /*frame_number*/, const std::vector<int32_t>& /*stream_ids*/) {
    return ::ndk::ScopedAStatus::ok();
}

void CameraDeviceSession::Run() {
    while (true) {
        PendingRequest request;
        bool discard;
        uint8_t depth;
        {
            std::unique_lock<std::mutex> lock(lock_);
            changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;
            request = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
            discard = flushing_ || stopping_;
            // Requests queued behind this one, plus this one.
            depth = static_cast<uint8_t>(std::min<size_t>(queue_.size() + 1, kPipelineMaxDepth));
        }

        if (discard) {
            Fail(request, ErrorCode::ERROR_REQUEST);
        } else {
            Process(request, depth);
        }

        {
            std::lock_guard<std::mutex> lock(lock_);
            busy_ = false;
        }
        changed_.notify_all();
    }
}

void CameraDeviceSession::Process(PendingRequest& request, uint8_t pipeline_depth) {
    if (device_lost_) {
        Fail(request, ErrorCode::ERROR_REQUEST);
        return;
    }

    const RequestSettings parsed = ParseSettings(*request.settings, *description_);
    const FlashControl::Plan flash = flash_control_.Begin(
            parsed, std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    LightFlash(flash.lit);
    const ::android::base::Result<int64_t> captured = [&]() -> ::android::base::Result<int64_t> {
        if (auto prepared = capture_.Prepare(parsed); !prepared.ok()) return prepared.error();
        return capture_.Capture(&frame_, flash.not_before_ns);
    }();
    if (flash.lit && !flash.keep_lit) {
        // Fired for this frame only.
        LightFlash(false);
        flash_control_.Unlit();
    }
    if (!captured.ok()) {
        LOG(ERROR) << name_ << ": request " << request.frame_number << ": "
                   << captured.error().message();
        if (captured.error().code().value() == ENODEV) {
            // The device is gone for good.
            device_lost_ = true;
            NotifyMsg message;
            message.set<NotifyMsg::Tag::error>(ErrorMsg{
                    .frameNumber = 0, .errorStreamId = -1, .errorCode = ErrorCode::ERROR_DEVICE});
            Notify({message});
        }
        Fail(request, ErrorCode::ERROR_REQUEST);
        return;
    }
    const int64_t timestamp = *captured;
    if (flash_control_.WantsBrightness(parsed)) flash_control_.SetBrightness(MeanLuma(frame_));

    NotifyMsg shutter;
    shutter.set<NotifyMsg::Tag::shutter>(ShutterMsg{.frameNumber = request.frame_number,
                                                    .timestamp = timestamp,
                                                    .readoutTimestamp = timestamp});
    Notify({shutter});

    // Camera privacy: every output of the request is black.
    if (parsed.black) FillBlack(&frame_);

    const Rect region =
            ToCaptureCoordinates(parsed.region, description_->active_array(), capture_.size());
    CaptureResult result;
    result.frameNumber = request.frame_number;
    result.inputBuffer.streamId = -1;
    result.partialResult = 1;
    std::vector<NotifyMsg> errors;
    for (auto& output : request.outputs) {
        const bool ok = WriteOutput(output, frame_, region, *request.settings);
        if (!ok) {
            NotifyMsg error;
            error.set<NotifyMsg::Tag::error>(ErrorMsg{.frameNumber = request.frame_number,
                                                      .errorStreamId = output.stream_id,
                                                      .errorCode = ErrorCode::ERROR_BUFFER});
            errors.push_back(std::move(error));
        }
        result.outputBuffers.push_back(ReturnBuffer(output, ok));
    }
    if (!errors.empty()) Notify(errors);

    Metadata metadata = BuildResult(*request.settings, parsed, timestamp, pipeline_depth);
    metadata.SetU8(ANDROID_CONTROL_AE_STATE, flash.ae_state);
    metadata.SetU8(ANDROID_FLASH_STATE, flash.flash_state);
    SendResult(std::move(result), &metadata);
}

void CameraDeviceSession::LightFlash(bool lit) {
    if (flash_ == nullptr || lit == flash_lit_) return;
    flash_->SetLit(lit);
    flash_lit_ = lit;
}

bool CameraDeviceSession::WriteOutput(OutputBuffer& output, const I420Image& image,
                                      const Rect& region, const Metadata& settings) {
    if (output.acquire_fence.get() >= 0) {
        if (sync_wait(output.acquire_fence.get(), kAcquireFenceTimeoutMs) != 0) {
            PLOG(ERROR) << name_ << ": buffer " << output.buffer_id << " of stream "
                        << output.stream_id << " not released in time";
            return false;
        }
        output.acquire_fence.reset();
    }

    const Size size = output.stream.size;
    bool ok = false;
    switch (output.stream.format) {
        case PixelFormat::YCBCR_420_888:
            if (auto destination = buffers_->LockYuv(output.handle, size)) {
                ok = ScaleToYuv(image, CenterCropToAspect(region, size), size, *destination,
                                &scratch_);
                buffers_->Unlock(output.handle);
            }
            break;
        case PixelFormat::RGBA_8888:
            if (auto destination = buffers_->Lock(output.handle, size)) {
                ok = ScaleToRgba(image, CenterCropToAspect(region, size), size, *destination,
                                 &scratch_);
                buffers_->Unlock(output.handle);
            }
            break;
        case PixelFormat::BLOB: {
            // BLOB buffers are one row of buffer_size bytes.
            const Size bytes = {output.stream.buffer_size, 1};
            if (auto destination = buffers_->Lock(output.handle, bytes)) {
                ok = WriteJpeg(image, region, size, settings, jpeg_context_, destination->data,
                               static_cast<size_t>(output.stream.buffer_size), &jpeg_workspace_);
                buffers_->Unlock(output.handle);
            }
            break;
        }
        default:
            LOG(ERROR) << name_ << ": stream " << output.stream_id << ": unsupported format "
                       << toString(output.stream.format);
            break;
    }
    return ok;
}

StreamBuffer CameraDeviceSession::ReturnBuffer(OutputBuffer& output, bool ok) {
    StreamBuffer buffer;
    buffer.streamId = output.stream_id;
    buffer.bufferId = output.buffer_id;
    buffer.status = ok ? BufferStatus::OK : BufferStatus::ERROR;
    // A buffer that was never waited for still belongs to its previous
    // user: hand the acquire fence back as release fence.
    if (output.acquire_fence.get() >= 0) {
        buffer.releaseFence.fds.emplace_back(output.acquire_fence.release());
    }
    return buffer;
}

void CameraDeviceSession::Fail(PendingRequest& request, ErrorCode code) {
    NotifyMsg message;
    message.set<NotifyMsg::Tag::error>(
            ErrorMsg{.frameNumber = request.frame_number, .errorStreamId = -1, .errorCode = code});
    Notify({message});

    CaptureResult result;
    result.frameNumber = request.frame_number;
    result.inputBuffer.streamId = -1;
    result.partialResult = 0;
    for (auto& output : request.outputs) {
        result.outputBuffers.push_back(ReturnBuffer(output, false));
    }
    SendResult(std::move(result), nullptr);
}

void CameraDeviceSession::SendResult(CaptureResult result, const Metadata* metadata) {
    std::lock_guard<std::mutex> lock(result_lock_);
    if (metadata != nullptr) {
        device::CameraMetadata aidl = metadata->ToAidl();
        const size_t size = aidl.metadata.size();
        if (result_queue_ != nullptr && size <= result_queue_->availableToWrite() &&
            result_queue_->write(reinterpret_cast<const int8_t*>(aidl.metadata.data()), size)) {
            result.fmqResultSize = static_cast<int64_t>(size);
        } else {
            result.fmqResultSize = 0;
            result.result = std::move(aidl);
        }
    }
    std::vector<CaptureResult> results;
    results.push_back(std::move(result));
    const auto status = callback_->processCaptureResult(results);
    if (!status.isOk()) {
        LOG(ERROR) << name_ << ": processCaptureResult: " << status.getDescription();
    }
}

void CameraDeviceSession::Notify(const std::vector<NotifyMsg>& messages) {
    const auto status = callback_->notify(messages);
    if (!status.isOk()) LOG(ERROR) << name_ << ": notify: " << status.getDescription();
}

}  // namespace aidl::android::hardware::camera::mainline
