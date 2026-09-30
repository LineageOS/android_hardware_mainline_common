/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#include <aidl/android/hardware/camera/device/BnCameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/CameraBlob.h>
#include <aidl/android/hardware/camera/device/CameraBlobId.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <gtest/gtest.h>
#include <system/camera_metadata.h>

#include "device/RequestTemplates.h"
#include "session/CameraDeviceSession.h"
#include "tests/FakeGraphicBuffers.h"
#include "tests/FakeVideoDevice.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::aidl::android::hardware::graphics::common::Dataspace;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using device::BufferRequest;
using device::BufferRequestStatus;
using device::BufferStatus;
using device::CaptureRequest;
using device::CaptureResult;
using device::ErrorCode;
using device::HalStream;
using device::NotifyMsg;
using device::RequestTemplate;
using device::Stream;
using device::StreamBuffer;
using device::StreamBufferRet;
using device::StreamConfiguration;

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// The parts of a CaptureResult the tests look at. CaptureResult itself can not
// be copied: its buffers may hold file descriptors.
struct RecordedResult {
    struct Buffer {
        int32_t streamId;
        int64_t bufferId;
        BufferStatus status;
    };
    int32_t frameNumber;
    int64_t fmqResultSize;
    device::CameraMetadata result;
    int32_t partialResult;
    std::vector<Buffer> outputBuffers;
};

class FakeCallback : public device::BnCameraDeviceCallback {
  public:
    ::ndk::ScopedAStatus notify(const std::vector<NotifyMsg>& messages) override {
        std::lock_guard<std::mutex> lock(lock_);
        messages_.insert(messages_.end(), messages.begin(), messages.end());
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus processCaptureResult(const std::vector<CaptureResult>& results) override {
        {
            std::lock_guard<std::mutex> lock(lock_);
            for (const auto& result : results) {
                RecordedResult recorded = {result.frameNumber,
                                           result.fmqResultSize,
                                           result.result,
                                           result.partialResult,
                                           {}};
                for (const auto& buffer : result.outputBuffers) {
                    recorded.outputBuffers.push_back(
                            {buffer.streamId, buffer.bufferId, buffer.status});
                }
                results_.push_back(std::move(recorded));
            }
        }
        changed_.notify_all();
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus requestStreamBuffers(const std::vector<BufferRequest>& /*requests*/,
                                              std::vector<StreamBufferRet>* /*buffers*/,
                                              BufferRequestStatus* status) override {
        *status = BufferRequestStatus::FAILED_UNKNOWN;
        return ::ndk::ScopedAStatus::ok();
    }

    ::ndk::ScopedAStatus returnStreamBuffers(
            const std::vector<StreamBuffer>& /*buffers*/) override {
        return ::ndk::ScopedAStatus::ok();
    }

    // Waits until `count` results arrived.
    bool WaitForResults(size_t count) {
        std::unique_lock<std::mutex> lock(lock_);
        return changed_.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return results_.size() >= count; });
    }

    std::vector<NotifyMsg> messages() {
        std::lock_guard<std::mutex> lock(lock_);
        return messages_;
    }
    std::vector<RecordedResult> results() {
        std::lock_guard<std::mutex> lock(lock_);
        return results_;
    }

    bool HasError(int32_t frame, ErrorCode code) {
        for (const auto& message : messages()) {
            if (message.getTag() != NotifyMsg::Tag::error) continue;
            const auto& error = message.get<NotifyMsg::Tag::error>();
            if (error.errorCode == code && (frame < 0 || error.frameNumber == frame)) return true;
        }
        return false;
    }

  private:
    std::mutex lock_;
    std::condition_variable changed_;
    std::vector<NotifyMsg> messages_;
    std::vector<RecordedResult> results_;
};

Stream MakeStream(int id, int width, int height, PixelFormat format,
                  BufferUsage usage = static_cast<BufferUsage>(0)) {
    Stream stream;
    stream.id = id;
    stream.streamType = device::StreamType::OUTPUT;
    stream.width = width;
    stream.height = height;
    stream.format = format;
    stream.usage = usage;
    stream.dataSpace = Dataspace::UNKNOWN;
    stream.rotation = device::StreamRotation::ROTATION_0;
    stream.groupId = -1;
    stream.colorSpace = -1;
    return stream;
}

StreamConfiguration Config(std::vector<Stream> streams) {
    StreamConfiguration config;
    config.streams = std::move(streams);
    config.operationMode = device::StreamConfigurationMode::NORMAL_MODE;
    return config;
}

// Request parts can not be copied (they may hold file descriptors), so
// vectors of them are built by moving.
template <typename... Buffers>
std::vector<StreamBuffer> BufferList(Buffers&&... buffers) {
    std::vector<StreamBuffer> list;
    (list.push_back(std::forward<Buffers>(buffers)), ...);
    return list;
}

StreamBuffer Buffer(int stream, int64_t id, bool with_handle = true) {
    StreamBuffer buffer;
    buffer.streamId = stream;
    buffer.bufferId = id;
    if (with_handle) buffer.buffer = FakeGraphicBuffers::Handle(static_cast<int>(id));
    return buffer;
}

class SessionTest : public ::testing::Test {
  protected:
    void SetUp() override { Open(false); }

    void Open(bool prefer_rgb) {
        CameraCandidate candidate;
        candidate.key = "/sys/devices/test";
        candidate.info = FakeVideoDevice::UvcInfo("video0", candidate.key);
        candidate.formats = {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, kWidth, kHeight)};
        candidate.prefer_rgb = prefer_rgb;
        description_ = CameraDescription::Create(candidate);
        ASSERT_NE(description_, nullptr);

        // A YUYV frame of Y 100, U 50, V 200.
        std::vector<uint8_t> frame;
        for (int i = 0; i < kWidth * kHeight / 2; ++i)
            frame.insert(frame.end(), {100, 50, 100, 200});
        stream_->frames = {frame};

        callback_ = ::ndk::SharedRefBase::make<FakeCallback>();
        buffers_ = std::make_shared<FakeGraphicBuffers>();
        Status status;
        auto info = candidate.info;
        auto formats = candidate.formats;
        auto stream = stream_;
        session_ = CameraDeviceSession::Create(
                "test", description_, callback_,
                [=](const std::string&) -> ::android::base::Result<std::unique_ptr<VideoDevice>> {
                    return std::make_unique<FakeVideoDevice>(info, formats, stream);
                },
                buffers_, &status);
        ASSERT_EQ(status, Status::OK);
        ASSERT_NE(session_, nullptr);
    }

    void TearDown() override {
        if (session_ != nullptr) EXPECT_TRUE(session_->close().isOk());
    }

    CaptureRequest Request(int32_t frame, std::vector<StreamBuffer> buffers, bool settings = true) {
        CaptureRequest request;
        request.frameNumber = frame;
        request.inputBuffer.streamId = -1;
        if (settings) {
            request.settings =
                    BuildRequestTemplate(*description_, RequestTemplate::PREVIEW)->ToAidl();
        }
        request.outputBuffers = std::move(buffers);
        return request;
    }

    ::ndk::ScopedAStatus Submit(CaptureRequest request) {
        std::vector<CaptureRequest> requests;
        requests.push_back(std::move(request));
        int32_t processed = -1;
        auto status = session_->processCaptureRequest(requests, {}, &processed);
        if (status.isOk()) EXPECT_EQ(processed, 1);
        return status;
    }

    std::shared_ptr<const CameraDescription> description_;
    std::shared_ptr<FakeVideoDevice::Stream> stream_ = std::make_shared<FakeVideoDevice::Stream>();
    std::shared_ptr<FakeCallback> callback_;
    std::shared_ptr<FakeGraphicBuffers> buffers_;
    std::shared_ptr<CameraDeviceSession> session_;
};

TEST(SessionOpenTest, DeviceGone) {
    CameraCandidate candidate;
    candidate.key = "/sys/devices/test";
    candidate.info = FakeVideoDevice::UvcInfo("video0", candidate.key);
    candidate.formats = {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, kWidth, kHeight)};
    auto description = CameraDescription::Create(candidate);
    ASSERT_NE(description, nullptr);
    Status status = Status::OK;
    auto session = CameraDeviceSession::Create(
            "test", description, ::ndk::SharedRefBase::make<FakeCallback>(),
            [](const std::string&) -> ::android::base::Result<std::unique_ptr<VideoDevice>> {
                return ::android::base::Error(ENODEV) << "gone";
            },
            std::make_shared<FakeGraphicBuffers>(), &status);
    EXPECT_EQ(session, nullptr);
    EXPECT_EQ(status, Status::CAMERA_DISCONNECTED);
}

TEST_F(SessionTest, ConfigureStreams) {
    std::vector<HalStream> hal;
    ASSERT_TRUE(
            session_->configureStreams(
                            Config({MakeStream(0, kWidth, kHeight, PixelFormat::YCBCR_420_888),
                                    MakeStream(1, 320, 240, PixelFormat::IMPLEMENTATION_DEFINED)}),
                            &hal)
                    .isOk());
    ASSERT_EQ(hal.size(), 2u);
    for (const auto& stream : hal) {
        EXPECT_EQ(stream.overrideFormat, PixelFormat::YCBCR_420_888);
        EXPECT_GT(stream.maxBuffers, 0);
        EXPECT_NE(static_cast<int64_t>(stream.producerUsage) &
                          static_cast<int64_t>(BufferUsage::CPU_WRITE_OFTEN),
                  0);
    }

    // Unsupported size.
    EXPECT_FALSE(
            session_->configureStreams(
                            Config({MakeStream(0, 1280, 720, PixelFormat::YCBCR_420_888)}), &hal)
                    .isOk());
}

TEST_F(SessionTest, PreferRgb) {
    ASSERT_TRUE(session_->close().isOk());
    Open(true);
    std::vector<HalStream> hal;
    ASSERT_TRUE(session_->configureStreams(
                                Config({MakeStream(0, kWidth, kHeight,
                                                   PixelFormat::IMPLEMENTATION_DEFINED),
                                        MakeStream(1, 320, 240, PixelFormat::IMPLEMENTATION_DEFINED,
                                                   BufferUsage::VIDEO_ENCODER)}),
                                &hal)
                        .isOk());
    ASSERT_EQ(hal.size(), 2u);
    EXPECT_EQ(hal[0].overrideFormat, PixelFormat::RGBA_8888);
    // Video encoders get YUV anyway.
    EXPECT_EQ(hal[1].overrideFormat, PixelFormat::YCBCR_420_888);
}

TEST_F(SessionTest, CaptureYuv) {
    std::vector<HalStream> hal;
    ASSERT_TRUE(
            session_->configureStreams(
                            Config({MakeStream(0, kWidth, kHeight, PixelFormat::YCBCR_420_888),
                                    MakeStream(1, 320, 240, PixelFormat::IMPLEMENTATION_DEFINED)}),
                            &hal)
                    .isOk());

    ASSERT_TRUE(Submit(Request(1, BufferList(Buffer(0, 1), Buffer(1, 2)))).isOk());
    ASSERT_TRUE(callback_->WaitForResults(1));

    const auto results = callback_->results();
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].frameNumber, 1);
    EXPECT_EQ(results[0].partialResult, 1);
    EXPECT_TRUE(results[0].fmqResultSize > 0 || !results[0].result.metadata.empty());
    ASSERT_EQ(results[0].outputBuffers.size(), 2u);
    for (const auto& buffer : results[0].outputBuffers) {
        EXPECT_EQ(buffer.status, BufferStatus::OK);
    }

    // Shutter before the result, with a timestamp.
    const auto messages = callback_->messages();
    ASSERT_FALSE(messages.empty());
    ASSERT_EQ(messages[0].getTag(), NotifyMsg::Tag::shutter);
    EXPECT_EQ(messages[0].get<NotifyMsg::Tag::shutter>().frameNumber, 1);
    EXPECT_GT(messages[0].get<NotifyMsg::Tag::shutter>().timestamp, 0);

    // Both outputs got the frame's colour.
    for (const auto& [id, size] :
         {std::make_pair(1, Size{kWidth, kHeight}), std::make_pair(2, Size{320, 240})}) {
        const auto contents = buffers_->Contents(id);
        const size_t luma = static_cast<size_t>(size.width) * size.height;
        ASSERT_EQ(contents.size(), luma * 3 / 2);
        EXPECT_NEAR(contents[0], 100, 1);
        EXPECT_NEAR(contents[luma - 1], 100, 1);
        EXPECT_NEAR(contents[luma], 50, 1);
        EXPECT_NEAR(contents[luma + luma / 4], 200, 1);
    }

    // Cached buffers and the previous settings are reused.
    ASSERT_TRUE(Submit(Request(2, BufferList(Buffer(0, 1, false)), false)).isOk());
    ASSERT_TRUE(callback_->WaitForResults(2));
    EXPECT_EQ(callback_->results()[1].outputBuffers[0].status, BufferStatus::OK);
    EXPECT_EQ(buffers_->imported(), 2);

    ASSERT_TRUE(session_->close().isOk());
    EXPECT_EQ(buffers_->freed(), 2);
    session_ = nullptr;
}

TEST_F(SessionTest, CaptureJpeg) {
    constexpr int32_t kBufferSize = 512 * 1024;
    auto blob = MakeStream(0, kWidth, kHeight, PixelFormat::BLOB);
    blob.dataSpace = Dataspace::JFIF;
    blob.bufferSize = kBufferSize;
    std::vector<HalStream> hal;
    ASSERT_TRUE(session_->configureStreams(
                                Config({blob, MakeStream(1, 320, 240, PixelFormat::YCBCR_420_888)}),
                                &hal)
                        .isOk());
    EXPECT_EQ(hal[0].overrideFormat, PixelFormat::BLOB);

    ASSERT_TRUE(Submit(Request(1, BufferList(Buffer(0, 1), Buffer(1, 2)))).isOk());
    ASSERT_TRUE(callback_->WaitForResults(1));
    const auto result = callback_->results()[0];
    ASSERT_EQ(result.outputBuffers.size(), 2u);
    EXPECT_EQ(result.outputBuffers[0].status, BufferStatus::OK);
    EXPECT_EQ(result.outputBuffers[1].status, BufferStatus::OK);
    EXPECT_FALSE(callback_->HasError(1, ErrorCode::ERROR_BUFFER));

    const auto contents = buffers_->Contents(1);
    ASSERT_GE(contents.size(), static_cast<size_t>(kBufferSize));
    device::CameraBlob trailer;
    memcpy(&trailer, contents.data() + kBufferSize - sizeof(trailer), sizeof(trailer));
    EXPECT_EQ(trailer.blobId, device::CameraBlobId::JPEG);
    ASSERT_GT(trailer.blobSizeBytes, 0);
    EXPECT_EQ(contents[0], 0xff);
    EXPECT_EQ(contents[1], 0xd8);
}

TEST_F(SessionTest, TestPatternBlack) {
    std::vector<HalStream> hal;
    ASSERT_TRUE(session_->configureStreams(Config({MakeStream(0, kWidth, kHeight,
                                                              PixelFormat::YCBCR_420_888)}),
                                           &hal)
                        .isOk());
    auto request = Request(1, BufferList(Buffer(0, 1)));
    auto settings = Metadata::FromAidl(request.settings);
    ASSERT_TRUE(settings.has_value());
    settings->SetI32(ANDROID_SENSOR_TEST_PATTERN_MODE, ANDROID_SENSOR_TEST_PATTERN_MODE_BLACK);
    request.settings = settings->ToAidl();
    ASSERT_TRUE(Submit(std::move(request)).isOk());
    ASSERT_TRUE(callback_->WaitForResults(1));
    const auto contents = buffers_->Contents(1);
    ASSERT_FALSE(contents.empty());
    EXPECT_EQ(contents[0], 0);
    EXPECT_EQ(contents[kWidth * kHeight], 128);
}

TEST_F(SessionTest, RejectsBadRequests) {
    std::vector<HalStream> hal;
    // Not configured yet.
    EXPECT_FALSE(Submit(Request(1, BufferList(Buffer(0, 1)))).isOk());

    ASSERT_TRUE(session_->configureStreams(Config({MakeStream(0, kWidth, kHeight,
                                                              PixelFormat::YCBCR_420_888)}),
                                           &hal)
                        .isOk());
    // No settings in the first request.
    EXPECT_FALSE(Submit(Request(1, BufferList(Buffer(0, 1)), false)).isOk());
    // Unknown stream.
    EXPECT_FALSE(Submit(Request(1, BufferList(Buffer(5, 1)))).isOk());
    // Unknown buffer without a handle.
    EXPECT_FALSE(Submit(Request(1, BufferList(Buffer(0, 9, false)))).isOk());
    // No buffers.
    EXPECT_FALSE(Submit(Request(1, BufferList())).isOk());
    // Reprocessing.
    auto reprocess = Request(1, BufferList(Buffer(0, 1)));
    reprocess.inputBuffer.streamId = 0;
    EXPECT_FALSE(Submit(std::move(reprocess)).isOk());

    EXPECT_TRUE(callback_->results().empty());
}

TEST_F(SessionTest, DeviceLost) {
    std::vector<HalStream> hal;
    ASSERT_TRUE(session_->configureStreams(Config({MakeStream(0, kWidth, kHeight,
                                                              PixelFormat::YCBCR_420_888)}),
                                           &hal)
                        .isOk());
    {
        std::lock_guard<std::mutex> lock(stream_->lock);
        stream_->dequeue_error = ENODEV;
    }
    ASSERT_TRUE(Submit(Request(1, BufferList(Buffer(0, 1)))).isOk());
    ASSERT_TRUE(callback_->WaitForResults(1));
    EXPECT_TRUE(callback_->HasError(-1, ErrorCode::ERROR_DEVICE));
    EXPECT_TRUE(callback_->HasError(1, ErrorCode::ERROR_REQUEST));
    EXPECT_EQ(callback_->results()[0].outputBuffers[0].status, BufferStatus::ERROR);

    // Later requests fail too, without touching the device again.
    ASSERT_TRUE(Submit(Request(2, BufferList(Buffer(0, 1, false)))).isOk());
    ASSERT_TRUE(callback_->WaitForResults(2));
    EXPECT_TRUE(callback_->HasError(2, ErrorCode::ERROR_REQUEST));
}

TEST_F(SessionTest, CaptureTimeout) {
    std::vector<HalStream> hal;
    ASSERT_TRUE(session_->configureStreams(Config({MakeStream(0, kWidth, kHeight,
                                                              PixelFormat::YCBCR_420_888)}),
                                           &hal)
                        .isOk());
    {
        std::lock_guard<std::mutex> lock(stream_->lock);
        stream_->dequeue_error = ETIMEDOUT;
    }
    ASSERT_TRUE(Submit(Request(1, BufferList(Buffer(0, 1)))).isOk());
    ASSERT_TRUE(callback_->WaitForResults(1));
    EXPECT_FALSE(callback_->HasError(-1, ErrorCode::ERROR_DEVICE));
    EXPECT_TRUE(callback_->HasError(1, ErrorCode::ERROR_REQUEST));

    // The device recovers.
    {
        std::lock_guard<std::mutex> lock(stream_->lock);
        stream_->dequeue_error = 0;
    }
    ASSERT_TRUE(Submit(Request(2, BufferList(Buffer(0, 1, false)))).isOk());
    ASSERT_TRUE(callback_->WaitForResults(2));
    EXPECT_EQ(callback_->results()[1].outputBuffers[0].status, BufferStatus::OK);
}

TEST_F(SessionTest, FlushAndClose) {
    std::vector<HalStream> hal;
    ASSERT_TRUE(session_->configureStreams(Config({MakeStream(0, kWidth, kHeight,
                                                              PixelFormat::YCBCR_420_888)}),
                                           &hal)
                        .isOk());
    for (int frame = 1; frame <= 3; ++frame) {
        ASSERT_TRUE(Submit(Request(frame, BufferList(Buffer(0, frame)))).isOk());
    }
    ASSERT_TRUE(session_->flush().isOk());
    // Every request came back, processed or not.
    EXPECT_EQ(callback_->results().size(), 3u);

    ASSERT_TRUE(session_->close().isOk());
    EXPECT_TRUE(session_->close().isOk());
    EXPECT_TRUE(session_->IsClosed());
    EXPECT_FALSE(Submit(Request(4, BufferList(Buffer(0, 1, false)))).isOk());
    session_ = nullptr;
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
