/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// In-memory VideoDevice for the unit tests.
class FakeVideoDevice : public VideoDevice {
  public:
    // What the fake streams; shared with the test so that it can be changed
    // while a session owns the device.
    struct Stream {
        std::mutex lock;
        // Content of the frames, handed out in turn (the last one repeats).
        std::vector<std::vector<uint8_t>> frames;
        size_t next = 0;
        // errno value DequeueFrame() fails with, 0 for none.
        int dequeue_error = 0;
        // Recorded calls.
        std::optional<CaptureFormat> format;
        std::optional<Fraction> interval;
        int starts = 0;
        int dequeued = 0;
    };

    FakeVideoDevice(VideoDeviceInfo info, std::vector<FormatDescription> formats,
                    std::shared_ptr<Stream> stream = std::make_shared<Stream>())
        : info_(std::move(info)), formats_(std::move(formats)), stream_(std::move(stream)) {}

    const VideoDeviceInfo& Info() const override { return info_; }
    // With a media bus code, the formats set with SetFormatsForCode().
    std::vector<FormatDescription> EnumerateFormats(uint32_t mbus_code) override {
        auto it = formats_by_code_.find(mbus_code);
        return it != formats_by_code_.end() ? it->second : formats_;
    }
    void SetFormatsForCode(uint32_t code, std::vector<FormatDescription> formats) {
        formats_by_code_[code] = std::move(formats);
    }

    bool HasControl(uint32_t id) override { return controls_.count(id) != 0; }
    std::optional<int32_t> GetControl(uint32_t id) override;
    bool SetControl(uint32_t id, int32_t value) override;

    ::android::base::Result<CaptureFormat> SetFormat(uint32_t fourcc, uint32_t width,
                                                     uint32_t height) override;
    ::android::base::Result<Fraction> SetFrameInterval(const Fraction& interval) override;
    ::android::base::Result<void> StartStreaming(uint32_t buffer_count) override;
    void StopStreaming() override { streaming_ = false; }
    bool IsStreaming() const override { return streaming_; }
    ::android::base::Result<CapturedFrame> DequeueFrame(std::chrono::milliseconds timeout) override;
    ::android::base::Result<void> QueueFrame(uint32_t index) override;

    std::map<uint32_t, int32_t>& controls() { return controls_; }

    // A UVC like capture node: "videoN" of a USB camera.
    static VideoDeviceInfo UvcInfo(const std::string& name, const std::string& sysfs_device);
    // One format with a single size and 30 fps.
    static FormatDescription Format(uint32_t fourcc, uint32_t width, uint32_t height);

  private:
    VideoDeviceInfo info_;
    std::vector<FormatDescription> formats_;
    std::map<uint32_t, std::vector<FormatDescription>> formats_by_code_;
    std::shared_ptr<Stream> stream_;
    std::map<uint32_t, int32_t> controls_;
    bool streaming_ = false;
    // The frame handed out last, kept alive until the next one.
    std::vector<uint8_t> current_;
};

}  // namespace aidl::android::hardware::camera::mainline
