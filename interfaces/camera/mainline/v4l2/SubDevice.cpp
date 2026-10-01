/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_SubDevice"

#include "v4l2/SubDevice.h"

#include <fcntl.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <unistd.h>

#include <algorithm>

#include <android-base/logging.h>
#include <android-base/unique_fd.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::android::base::ErrnoError;
using ::android::base::Error;
using ::android::base::Result;
using ::android::base::unique_fd;

bool IntervalLess(const Fraction& a, const Fraction& b) {
    return static_cast<uint64_t>(a.numerator) * b.denominator <
           static_cast<uint64_t>(b.numerator) * a.denominator;
}

class V4l2SubDevice : public SubDevice {
  public:
    V4l2SubDevice(unique_fd fd, std::string path) : fd_(std::move(fd)), path_(std::move(path)) {}

    const std::string& Path() const override { return path_; }

    bool HasControl(uint32_t id) override { return HasV4l2Control(fd_.get(), id); }
    std::optional<int32_t> GetControl(uint32_t id) override {
        return GetV4l2Control(fd_.get(), id);
    }
    std::optional<ControlRange> GetControlRange(uint32_t id) override {
        return GetV4l2ControlRange(fd_.get(), id);
    }
    bool SetControl(uint32_t id, int32_t value) override {
        return SetV4l2Control(fd_.get(), path_.c_str(), id, value);
    }

    std::vector<uint32_t> EnumerateCodes(uint32_t pad) override {
        std::vector<uint32_t> codes;
        for (uint32_t index = 0;; ++index) {
            v4l2_subdev_mbus_code_enum code = {};
            code.pad = pad;
            code.index = index;
            code.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            if (Xioctl(fd_.get(), VIDIOC_SUBDEV_ENUM_MBUS_CODE, &code) != 0) break;
            codes.push_back(code.code);
        }
        return codes;
    }

    std::vector<FrameSize> EnumerateSizes(uint32_t pad, uint32_t code) override {
        std::vector<FrameSize> sizes;
        for (uint32_t index = 0;; ++index) {
            v4l2_subdev_frame_size_enum size = {};
            size.pad = pad;
            size.index = index;
            size.code = code;
            size.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            if (Xioctl(fd_.get(), VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &size) != 0) break;
            // Sensors list discrete modes (min == max); take the maximum of a
            // range, which is the full mode.
            FrameSize frame = {size.max_width, size.max_height, {}};
            frame.intervals = EnumerateIntervals(pad, code, frame.width, frame.height);
            sizes.push_back(std::move(frame));
        }
        return sizes;
    }

    Result<MbusFormat> GetFormat(uint32_t pad) override {
        v4l2_subdev_format format = {};
        format.pad = pad;
        format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        if (Xioctl(fd_.get(), VIDIOC_SUBDEV_G_FMT, &format) != 0) {
            return ErrnoError() << path_ << ": VIDIOC_SUBDEV_G_FMT pad " << pad;
        }
        return MbusFormat{format.format.code, format.format.width, format.format.height};
    }

    Result<MbusFormat> SetFormat(uint32_t pad, const MbusFormat& requested) override {
        v4l2_subdev_format format = {};
        format.pad = pad;
        format.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        format.format.code = requested.code;
        format.format.width = requested.width;
        format.format.height = requested.height;
        format.format.field = V4L2_FIELD_NONE;
        if (Xioctl(fd_.get(), VIDIOC_SUBDEV_S_FMT, &format) != 0) {
            return ErrnoError() << path_ << ": VIDIOC_SUBDEV_S_FMT pad " << pad << " code 0x"
                                << std::hex << requested.code << std::dec << " " << requested.width
                                << "x" << requested.height;
        }
        return MbusFormat{format.format.code, format.format.width, format.format.height};
    }

    Result<Fraction> SetFrameInterval(uint32_t pad, const Fraction& requested) override {
        v4l2_subdev_frame_interval interval = {};
        interval.pad = pad;
        interval.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        interval.interval.numerator = requested.numerator;
        interval.interval.denominator = requested.denominator;
        if (Xioctl(fd_.get(), VIDIOC_SUBDEV_S_FRAME_INTERVAL, &interval) != 0) {
            return ErrnoError() << path_ << ": VIDIOC_SUBDEV_S_FRAME_INTERVAL";
        }
        return Fraction{interval.interval.numerator, interval.interval.denominator};
    }

  private:
    std::vector<Fraction> EnumerateIntervals(uint32_t pad, uint32_t code, uint32_t width,
                                             uint32_t height) {
        std::vector<Fraction> intervals;
        for (uint32_t index = 0;; ++index) {
            v4l2_subdev_frame_interval_enum interval = {};
            interval.pad = pad;
            interval.index = index;
            interval.code = code;
            interval.width = width;
            interval.height = height;
            interval.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            if (Xioctl(fd_.get(), VIDIOC_SUBDEV_ENUM_FRAME_INTERVAL, &interval) != 0) break;
            if (interval.interval.numerator == 0 || interval.interval.denominator == 0) continue;
            intervals.push_back({interval.interval.numerator, interval.interval.denominator});
        }
        std::sort(intervals.begin(), intervals.end(), IntervalLess);
        return intervals;
    }

    unique_fd fd_;
    std::string path_;
};

}  // namespace

bool IsBayerMbusCode(uint32_t code) {
    // MEDIA_BUS_FMT_SBGGR8_1X8 (0x3001) and following.
    return (code & 0xf000) == 0x3000;
}

Result<std::unique_ptr<SubDevice>> OpenSubDevice(const std::string& path) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDWR | O_CLOEXEC)));
    if (fd.get() < 0) return ErrnoError() << "open " << path;
    std::unique_ptr<SubDevice> device = std::make_unique<V4l2SubDevice>(std::move(fd), path);
    return device;
}

}  // namespace aidl::android::hardware::camera::mainline
