/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <android-base/unique_fd.h>
#include <fmq/AidlMessageQueue.h>

#include "convert/Image.h"
#include "device/CameraDescription.h"
#include "jpeg/JpegOutput.h"
#include "session/CaptureStream.h"
#include "session/GraphicBuffers.h"
#include "utils/Metadata.h"

namespace aidl::android::hardware::camera::mainline {

// A capture session: accepts requests on binder threads and processes them
// in order on its own worker thread, one captured frame per request.
class CameraDeviceSession : public device::BnCameraDeviceSession {
  public:
    // Opens the capture device. Returns nullptr (and the reason in `status`)
    // on failure.
    static std::shared_ptr<CameraDeviceSession> Create(
            std::string name, std::shared_ptr<const CameraDescription> description,
            std::shared_ptr<device::ICameraDeviceCallback> callback, const VideoDeviceOpener& open,
            std::shared_ptr<GraphicBuffers> buffers,
            ::aidl::android::hardware::camera::common::Status* status);

    // Use Create(); public for ndk::SharedRefBase::make() only.
    CameraDeviceSession(std::string name, std::shared_ptr<const CameraDescription> description,
                        std::shared_ptr<device::ICameraDeviceCallback> callback,
                        std::unique_ptr<VideoDevice> device,
                        std::shared_ptr<GraphicBuffers> buffers);
    ~CameraDeviceSession() override;

    bool IsClosed();

    // ICameraDeviceSession
    ::ndk::ScopedAStatus close() override;
    ::ndk::ScopedAStatus configureStreams(const device::StreamConfiguration& config,
                                          std::vector<device::HalStream>* streams) override;
    ::ndk::ScopedAStatus constructDefaultRequestSettings(device::RequestTemplate type,
                                                         device::CameraMetadata* settings) override;
    ::ndk::ScopedAStatus flush() override;
    ::ndk::ScopedAStatus getCaptureRequestMetadataQueue(
            ::aidl::android::hardware::common::fmq::MQDescriptor<
                    int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>* queue)
            override;
    ::ndk::ScopedAStatus getCaptureResultMetadataQueue(
            ::aidl::android::hardware::common::fmq::MQDescriptor<
                    int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>* queue)
            override;
    ::ndk::ScopedAStatus isReconfigurationRequired(const device::CameraMetadata& old_params,
                                                   const device::CameraMetadata& new_params,
                                                   bool* required) override;
    ::ndk::ScopedAStatus processCaptureRequest(const std::vector<device::CaptureRequest>& requests,
                                               const std::vector<device::BufferCache>& caches,
                                               int32_t* processed) override;
    ::ndk::ScopedAStatus signalStreamFlush(const std::vector<int32_t>& stream_ids,
                                           int32_t stream_config_counter) override;
    ::ndk::ScopedAStatus switchToOffline(
            const std::vector<int32_t>& streams, device::CameraOfflineSessionInfo* info,
            std::shared_ptr<device::ICameraOfflineSession>* session) override;
    ::ndk::ScopedAStatus repeatingRequestEnd(int32_t frame_number,
                                             const std::vector<int32_t>& stream_ids) override;
    ::ndk::ScopedAStatus configureStreamsV2(const device::StreamConfiguration& config,
                                            device::ConfigureStreamsRet* result) override;

  private:
    using MetadataQueue = ::android::AidlMessageQueue<
            int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>;

    // A configured output stream, with the format the buffers really have.
    struct OutputStream {
        Size size;
        ::aidl::android::hardware::graphics::common::PixelFormat format;
        // Bytes of a BLOB (JPEG) buffer, 0 for other formats.
        int32_t buffer_size = 0;
    };

    // One output buffer of a request.
    struct OutputBuffer {
        int32_t stream_id = -1;
        int64_t buffer_id = 0;
        OutputStream stream;
        buffer_handle_t handle = nullptr;
        ::android::base::unique_fd acquire_fence;
    };

    struct PendingRequest {
        int32_t frame_number = 0;
        std::shared_ptr<const Metadata> settings;
        std::vector<OutputBuffer> outputs;
    };

    // Checks a request and turns it into a PendingRequest; binder thread.
    ::aidl::android::hardware::camera::common::Status Prepare(const device::CaptureRequest& request,
                                                              PendingRequest* pending);
    void FreeBuffersLocked(const std::vector<device::BufferCache>& caches);
    void FreeAllBuffersLocked();

    // Worker thread.
    void Run();
    void Process(PendingRequest& request, uint8_t pipeline_depth);
    // Returns all buffers of a request with an error.
    void Fail(PendingRequest& request, device::ErrorCode code);
    // Writes one output; false if the buffer has to be returned with an error.
    bool WriteOutput(OutputBuffer& output, const I420Image& image, const Rect& region,
                     const Metadata& settings);
    device::StreamBuffer ReturnBuffer(OutputBuffer& output, bool ok);
    void SendResult(device::CaptureResult result, const Metadata* metadata);
    void Notify(const std::vector<device::NotifyMsg>& messages);

    const std::string name_;
    const std::shared_ptr<const CameraDescription> description_;
    const std::shared_ptr<device::ICameraDeviceCallback> callback_;
    const std::shared_ptr<GraphicBuffers> buffers_;
    std::unique_ptr<MetadataQueue> request_queue_;
    std::unique_ptr<MetadataQueue> result_queue_;

    // Everything below that is shared between binder threads and the worker.
    std::mutex lock_;
    std::condition_variable changed_;
    bool closed_ = false;
    bool stopping_ = false;
    bool flushing_ = false;
    bool busy_ = false;
    std::deque<PendingRequest> queue_;
    std::map<int32_t, OutputStream> streams_;
    // Imported buffers by stream ID and buffer ID.
    std::map<int32_t, std::map<int64_t, buffer_handle_t>> buffer_cache_;
    // Settings of the last request, used when a request has none.
    std::shared_ptr<const Metadata> last_settings_;

    // Only used by the worker thread (and configureStreams(), while the
    // worker is idle).
    CaptureStream capture_;
    I420Image frame_;
    I420Image scratch_;
    JpegContext jpeg_context_;
    JpegWorkspace jpeg_workspace_;
    bool device_lost_ = false;

    // Serializes results written to result_queue_ and sent to the framework.
    std::mutex result_lock_;

    std::thread worker_;
};

}  // namespace aidl::android::hardware::camera::mainline
