/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <vector>

#include "device/StreamPlanner.h"

namespace aidl::android::hardware::camera::mainline {

struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    bool operator==(const Rect& other) const {
        return x == other.x && y == other.y && width == other.width && height == other.height;
    }
};

// A planar YUV 4:2:0 image in memory owned by this object. Reused from frame
// to frame; Resize() only reallocates when growing.
class I420Image {
  public:
    void Resize(int32_t width, int32_t height);

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    Size size() const { return {width_, height_}; }

    uint8_t* y() { return data_.data(); }
    uint8_t* u() { return data_.data() + y_size(); }
    uint8_t* v() { return u() + uv_size(); }
    const uint8_t* y() const { return data_.data(); }
    const uint8_t* u() const { return data_.data() + y_size(); }
    const uint8_t* v() const { return u() + uv_size(); }
    int32_t y_stride() const { return width_; }
    int32_t uv_stride() const { return (width_ + 1) / 2; }

    // Full range (JPEG) rather than video range YCbCr.
    bool full_range = false;

  private:
    size_t y_size() const { return static_cast<size_t>(width_) * height_; }
    size_t uv_size() const { return static_cast<size_t>(uv_stride()) * ((height_ + 1) / 2); }

    int32_t width_ = 0;
    int32_t height_ = 0;
    std::vector<uint8_t> data_;
};

// Where to write a YUV 4:2:0 output: planar or semi-planar (either chroma
// order), as described by an android_ycbcr of a locked graphic buffer.
struct YuvDestination {
    uint8_t* y = nullptr;
    uint8_t* cb = nullptr;
    uint8_t* cr = nullptr;
    int32_t y_stride = 0;
    int32_t c_stride = 0;
    // 1: planar, 2: interleaved.
    int32_t c_step = 1;
};

// Where to write an RGBA 8888 output (bytes R, G, B, A in memory).
struct RgbaDestination {
    uint8_t* data = nullptr;
    int32_t stride_bytes = 0;
};

}  // namespace aidl::android::hardware::camera::mainline
