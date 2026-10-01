/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Capture"

#include "session/CaptureStream.h"

#include <time.h>

#include <algorithm>
#include <cerrno>

#include <android-base/logging.h>

#include "convert/FormatConverter.h"
#include "device/CameraDescription.h"
#include "v4l2/PixelFormats.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::android::base::Error;
using ::android::base::Result;

// Frames to skip at most (corrupt or undecodable) before giving up on a
// request.
constexpr int kMaxSkippedFrames = 4;

// Shortest time to wait for a frame; some devices take a while to deliver the
// first one after streaming starts.
constexpr std::chrono::milliseconds kMinFrameTimeout{1500};

int64_t Now(clockid_t clock) {
    timespec ts;
    clock_gettime(clock, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

bool SameInterval(const Fraction& a, const Fraction& b) {
    return static_cast<uint64_t>(a.numerator) * b.denominator ==
           static_cast<uint64_t>(b.numerator) * a.denominator;
}

}  // namespace

CaptureStream::CaptureStream(std::unique_ptr<VideoDevice> device,
                             std::unique_ptr<PipelineController> pipeline)
    : device_(std::move(device)),
      pipeline_(std::move(pipeline)),
      controls_(pipeline_ != nullptr ? pipeline_->sensor()
                                     : static_cast<ControlDevice*>(device_.get())) {}

CaptureStream::~CaptureStream() {
    Stop();
}

void CaptureStream::Configure(const CaptureMode& mode) {
    Stop();
    mode_ = mode;
    frame_size_ = mode.size;
    format_.reset();
    interval_ = {};
}

Result<void> CaptureStream::Prepare(const RequestSettings& settings) {
    const Fraction target = ChooseFrameInterval(mode_.intervals, settings.fps_range[1]);

    if (!device_->IsStreaming() || !SameInterval(target, interval_)) {
        device_->StopStreaming();
        if (!format_.has_value()) {
            frame_size_ = mode_.size;
            if (pipeline_ != nullptr) {
                auto size = pipeline_->Configure(mode_.fourcc, mode_.size);
                if (!size.ok()) return size.error();
                frame_size_ = *size;
            }
            auto format = device_->SetFormat(mode_.fourcc, static_cast<uint32_t>(frame_size_.width),
                                             static_cast<uint32_t>(frame_size_.height));
            if (!format.ok()) return format.error();
            format_ = *format;
            controls_.Reset();
        }
        // Behind a media controller the sensor sets the pace.
        auto applied = pipeline_ != nullptr ? pipeline_->SetFrameInterval(target)
                                            : device_->SetFrameInterval(target);
        if (applied.ok()) {
            LOG(DEBUG) << device_->Info().name << ": frame interval " << applied->numerator << "/"
                       << applied->denominator;
        } else {
            LOG(DEBUG) << applied.error().message();
        }
        interval_ = target;
        if (auto started = device_->StartStreaming(kPipelineMaxDepth); !started.ok()) {
            return started.error();
        }
        LOG(INFO) << device_->Info().name << ": capturing " << FourccToString(mode_.fourcc) << " "
                  << frame_size_.width << "x" << frame_size_.height << " at "
                  << target.denominator / std::max(target.numerator, 1u) << " fps";
    }

    controls_.SetConstantFrameRate(settings.fps_range[0] == settings.fps_range[1]);
    controls_.SetAeLock(settings.ae_lock);
    controls_.SetAwbLock(settings.awb_lock);
    return {};
}

Result<int64_t> CaptureStream::Capture(I420Image* image) {
    if (!format_.has_value() || !device_->IsStreaming()) {
        return Error(EINVAL) << device_->Info().name << ": not streaming";
    }
    const auto timeout = std::max(
            kMinFrameTimeout, std::chrono::milliseconds(3 * interval_.ToNanoseconds() / 1'000'000));

    for (int attempt = 0; attempt <= kMaxSkippedFrames; ++attempt) {
        auto frame = device_->DequeueFrame(timeout);
        if (!frame.ok()) return frame.error();

        const bool converted = ConvertToI420(*format_, *frame, image);
        int64_t timestamp = Now(CLOCK_BOOTTIME);
        if (frame->timestamp_ns > 0) {
            // V4L2 uses CLOCK_MONOTONIC, Android CLOCK_BOOTTIME.
            timestamp = frame->timestamp_ns + (timestamp - Now(CLOCK_MONOTONIC));
        }

        if (auto queued = device_->QueueFrame(frame->index); !queued.ok()) {
            if (queued.error().code().value() == ENODEV) return queued.error();
            LOG(WARNING) << queued.error().message();
        }
        if (converted) return timestamp;
    }
    return Error(EIO) << device_->Info().name << ": no usable frame";
}

void CaptureStream::Stop() {
    device_->StopStreaming();
}

}  // namespace aidl::android::hardware::camera::mainline
