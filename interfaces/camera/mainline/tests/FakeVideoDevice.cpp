/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tests/FakeVideoDevice.h"

#include <linux/videodev2.h>

namespace aidl::android::hardware::camera::mainline {

std::optional<int32_t> FakeVideoDevice::GetControl(uint32_t id) {
    auto it = controls_.find(id);
    if (it == controls_.end()) return std::nullopt;
    return it->second;
}

bool FakeVideoDevice::SetControl(uint32_t id, int32_t value) {
    auto it = controls_.find(id);
    if (it == controls_.end()) return false;
    it->second = value;
    return true;
}

VideoDeviceInfo FakeVideoDevice::UvcInfo(const std::string& name, const std::string& sysfs_device) {
    VideoDeviceInfo info;
    info.path = "/dev/" + name;
    info.name = name;
    info.driver = "uvcvideo";
    info.card = "HD Pro Webcam C920";
    info.bus_info = "usb-0000:00:14.0-6";
    info.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
    info.sysfs_device = sysfs_device;
    info.usb_vendor_id = 0x046d;
    info.usb_product_id = 0x082d;
    return info;
}

FormatDescription FakeVideoDevice::Format(uint32_t fourcc, uint32_t width, uint32_t height) {
    FormatDescription format;
    format.fourcc = fourcc;
    format.sizes.push_back({width, height, {{1, 30}}});
    return format;
}

}  // namespace aidl::android::hardware::camera::mainline
