/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

struct Size {
    int32_t width = 0;
    int32_t height = 0;

    int64_t Area() const { return static_cast<int64_t>(width) * height; }
    // Whether an image of `other` can be cut out of this one.
    bool Contains(const Size& other) const {
        return width >= other.width && height >= other.height;
    }
    bool operator==(const Size& other) const {
        return width == other.width && height == other.height;
    }
    bool operator!=(const Size& other) const { return !(*this == other); }
};

// A way to run the capture device: pixel format, frame size and the frame
// intervals it supports there.
struct CaptureMode {
    uint32_t fourcc = 0;
    Size size;
    bool emulated = false;
    // Shortest first.
    std::vector<Fraction> intervals;

    int64_t MinFrameDurationNs() const;
};

// An output size the camera offers to Android, and the shortest frame
// duration at which it can be produced.
struct OutputSize {
    Size size;
    int64_t min_frame_duration_ns = 0;
};

// Decides which output sizes a camera offers, and which capture mode feeds a
// given set of output streams.
//
// Every output is produced from a single capture stream by center cropping
// to the output's aspect ratio and scaling down, so an output can be served
// by any capture size that contains it.
class StreamPlanner {
  public:
    explicit StreamPlanner(const std::vector<FormatDescription>& formats);

    // Largest first.
    const std::vector<OutputSize>& OutputSizes() const { return output_sizes_; }
    bool IsOutputSize(const Size& size) const;
    // The largest capture size, i.e. the full field of view.
    Size MaxSize() const { return max_size_; }
    // Frame rates (frames per second) that some capture mode supports,
    // ascending.
    std::vector<int32_t> FrameRates() const;

    // Picks the capture mode for a set of outputs, or nullopt when there is
    // none that contains all of them.
    //
    // Preferred are, in order: 30 fps or more (up to 30 fps, a higher frame
    // rate wins), the smallest size, an uncompressed format, a format not
    // emulated by the driver.
    std::optional<CaptureMode> Plan(const std::vector<Size>& outputs) const;

    const std::vector<CaptureMode>& Modes() const { return modes_; }

  private:
    std::vector<CaptureMode> modes_;
    std::vector<OutputSize> output_sizes_;
    Size max_size_;
};

}  // namespace aidl::android::hardware::camera::mainline
