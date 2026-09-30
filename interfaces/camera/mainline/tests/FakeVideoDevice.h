/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <vector>

#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// In-memory VideoDevice for the unit tests.
class FakeVideoDevice : public VideoDevice {
  public:
    FakeVideoDevice(VideoDeviceInfo info, std::vector<FormatDescription> formats)
        : info_(std::move(info)), formats_(std::move(formats)) {}

    const VideoDeviceInfo& Info() const override { return info_; }
    std::vector<FormatDescription> EnumerateFormats() override { return formats_; }

    bool HasControl(uint32_t id) override { return controls_.count(id) != 0; }
    std::optional<int32_t> GetControl(uint32_t id) override;
    bool SetControl(uint32_t id, int32_t value) override;

    std::map<uint32_t, int32_t>& controls() { return controls_; }

    // A UVC like capture node: "videoN" of a USB camera.
    static VideoDeviceInfo UvcInfo(const std::string& name, const std::string& sysfs_device);
    // One format with a single size and 30 fps.
    static FormatDescription Format(uint32_t fourcc, uint32_t width, uint32_t height);

  private:
    VideoDeviceInfo info_;
    std::vector<FormatDescription> formats_;
    std::map<uint32_t, int32_t> controls_;
};

}  // namespace aidl::android::hardware::camera::mainline
