/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tests/FakeVideoDevice.h"

#include <linux/videodev2.h>

namespace aidl::android::hardware::camera::mainline {

using ::android::base::Error;
using ::android::base::Result;

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

Result<CaptureFormat> FakeVideoDevice::SetFormat(uint32_t fourcc, uint32_t width, uint32_t height) {
    if (streaming_) return Error(EBUSY) << "streaming";
    // Packed 16 bit formats only; enough for the tests.
    CaptureFormat format = {fourcc, width, height, {{width * 2, width * height * 2}}};
    std::lock_guard<std::mutex> lock(stream_->lock);
    stream_->format = format;
    return format;
}

Result<Fraction> FakeVideoDevice::SetFrameInterval(const Fraction& interval) {
    std::lock_guard<std::mutex> lock(stream_->lock);
    stream_->interval = interval;
    return interval;
}

Result<void> FakeVideoDevice::StartStreaming(uint32_t /*buffer_count*/) {
    std::lock_guard<std::mutex> lock(stream_->lock);
    if (!stream_->format.has_value()) return Error(EINVAL) << "no format";
    streaming_ = true;
    ++stream_->starts;
    return {};
}

Result<CapturedFrame> FakeVideoDevice::DequeueFrame(std::chrono::milliseconds /*timeout*/) {
    std::lock_guard<std::mutex> lock(stream_->lock);
    if (!streaming_) return Error(EINVAL) << "not streaming";
    if (stream_->dequeue_error != 0) return Error(stream_->dequeue_error) << "fake error";
    if (stream_->frames.empty()) return Error(ETIMEDOUT) << "no frames";

    current_ = stream_->frames[std::min(stream_->next, stream_->frames.size() - 1)];
    ++stream_->next;
    CapturedFrame frame;
    frame.index = 0;
    frame.sequence = static_cast<uint32_t>(stream_->dequeued++);
    frame.timestamp_ns = 1'000'000'000LL + frame.sequence * 33'333'333LL;
    frame.planes.push_back({current_.data(), current_.size()});
    return frame;
}

Result<void> FakeVideoDevice::QueueFrame(uint32_t /*index*/) {
    return {};
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
