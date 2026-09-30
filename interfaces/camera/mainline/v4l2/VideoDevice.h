/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <android-base/result.h>

namespace aidl::android::hardware::camera::mainline {

struct Fraction {
    uint32_t numerator = 0;
    uint32_t denominator = 0;

    // Frame duration in nanoseconds of a frame interval, 0 if invalid.
    int64_t ToNanoseconds() const;
};

struct FrameSize {
    uint32_t width = 0;
    uint32_t height = 0;
    // Supported frame intervals (1 / frame rate), shortest first.
    std::vector<Fraction> intervals;

    uint64_t Area() const { return static_cast<uint64_t>(width) * height; }
};

struct FormatDescription {
    uint32_t fourcc = 0;
    std::string description;
    // Emulated by the kernel or libv4l, e.g. a format converted in software
    // by the driver (V4L2_FMT_FLAG_EMULATED).
    bool emulated = false;
    std::vector<FrameSize> sizes;
};

// Identity of a video device node, gathered when it is opened.
struct VideoDeviceInfo {
    // "/dev/video0"
    std::string path;
    // "video0"
    std::string name;
    // Device number of the node, to detect a node that was replaced.
    dev_t rdev = 0;
    // From VIDIOC_QUERYCAP.
    std::string driver;
    std::string card;
    std::string bus_info;
    uint32_t device_caps = 0;
    bool multiplanar = false;
    // Canonical sysfs path of the parent device (e.g. the USB interface of a
    // UVC camera). Every node created by one function of a device shares it.
    // Empty when it can not be resolved.
    std::string sysfs_device;
    // USB vendor / product ID when the device sits on USB.
    std::optional<uint16_t> usb_vendor_id;
    std::optional<uint16_t> usb_product_id;
};

// A V4L2 video device node. Abstract so that everything above it can be
// tested against a fake device.
class VideoDevice {
  public:
    virtual ~VideoDevice() = default;

    virtual const VideoDeviceInfo& Info() const = 0;

    // All capture pixel formats with their frame sizes and intervals.
    virtual std::vector<FormatDescription> EnumerateFormats() = 0;

    // Integer controls. GetControl() returns nullopt when the control does
    // not exist or can not be read.
    virtual bool HasControl(uint32_t id) = 0;
    virtual std::optional<int32_t> GetControl(uint32_t id) = 0;
    virtual bool SetControl(uint32_t id, int32_t value) = 0;
};

// Opens a V4L2 video node. The error code of a failure is the errno of the
// failing call, so that callers can tell a node that is not accessible
// (yet) apart from one that is not a usable device.
::android::base::Result<std::unique_ptr<VideoDevice>> OpenVideoDevice(const std::string& path);

}  // namespace aidl::android::hardware::camera::mainline
