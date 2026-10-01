/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <android-base/result.h>

#include "v4l2/Controls.h"

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
    // Whether the USB port is removable (true) or built in (false), from
    // the firmware (ACPI _PLD / _UPC) or the hub descriptor. Unknown for
    // non-USB devices and when the firmware does not say.
    std::optional<bool> usb_removable;
};

// Layout of one memory plane of a capture buffer.
struct PlaneFormat {
    uint32_t bytes_per_line = 0;
    uint32_t size_image = 0;
};

// The format a capture device was set to.
struct CaptureFormat {
    uint32_t fourcc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<PlaneFormat> planes;
};

// A filled capture buffer. Valid until it is queued again.
struct CapturedFrame {
    uint32_t index = 0;
    struct Plane {
        const uint8_t* data = nullptr;
        size_t bytes_used = 0;
    };
    std::vector<Plane> planes;
    // CLOCK_MONOTONIC time of the frame in nanoseconds, 0 when the driver
    // does not provide one.
    int64_t timestamp_ns = 0;
    uint32_t sequence = 0;
    // The driver flagged the buffer as (possibly) corrupt.
    bool error = false;
};

// A V4L2 video device node. Abstract so that everything above it can be
// tested against a fake device.
class VideoDevice : public ControlDevice {
  public:
    virtual const VideoDeviceInfo& Info() const = 0;

    // All capture pixel formats with their frame sizes and intervals. For a
    // node behind a media controller pipeline (V4L2_CAP_IO_MC), `mbus_code`
    // restricts them to the formats the node can produce from that media bus
    // format; 0 for all.
    virtual std::vector<FormatDescription> EnumerateFormats(uint32_t mbus_code = 0) = 0;

    // Streaming. The format and frame interval can only be changed while not
    // streaming. The error code of a failure is an errno value; ENODEV means
    // that the device is gone.
    virtual ::android::base::Result<CaptureFormat> SetFormat(uint32_t fourcc, uint32_t width,
                                                             uint32_t height) = 0;
    // Returns the interval the driver actually applied.
    virtual ::android::base::Result<Fraction> SetFrameInterval(const Fraction& interval) = 0;
    // Allocates and queues `buffer_count` buffers and starts streaming.
    virtual ::android::base::Result<void> StartStreaming(uint32_t buffer_count) = 0;
    // Stops streaming and frees the buffers. Harmless when not streaming.
    virtual void StopStreaming() = 0;
    virtual bool IsStreaming() const = 0;
    // Waits for the next frame. Fails with ETIMEDOUT when none arrives in
    // time.
    virtual ::android::base::Result<CapturedFrame> DequeueFrame(
            std::chrono::milliseconds timeout) = 0;
    // Hands a dequeued buffer back to the driver.
    virtual ::android::base::Result<void> QueueFrame(uint32_t index) = 0;
};

// Opens video nodes. A parameter wherever the unit tests need a fake.
using VideoDeviceOpener = std::function<::android::base::Result<std::unique_ptr<VideoDevice>>(
        const std::string& path)>;

// Opens a V4L2 video node. The error code of a failure is the errno of the
// failing call, so that callers can tell a node that is not accessible
// (yet) apart from one that is not a usable device.
::android::base::Result<std::unique_ptr<VideoDevice>> OpenVideoDevice(const std::string& path);

}  // namespace aidl::android::hardware::camera::mainline
