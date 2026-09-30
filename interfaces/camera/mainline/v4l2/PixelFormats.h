/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>

namespace aidl::android::hardware::camera::mainline {

// How the HAL can use a V4L2 pixel format.
enum class PixelFormatClass {
    // Not usable at all.
    kUnsupported,
    // Processed image data that FormatConverter turns into Android buffers
    // (packed/planar YUV, RGB, grey, MJPEG).
    kProcessed,
    // Raw Bayer data. Needs a (software) ISP, which does not exist yet.
    kBayer,
};

PixelFormatClass ClassifyPixelFormat(uint32_t fourcc);

inline bool IsProcessedPixelFormat(uint32_t fourcc) {
    return ClassifyPixelFormat(fourcc) == PixelFormatClass::kProcessed;
}

// Whether the format is compressed (MJPEG/JPEG). Compressed formats are
// preferred least when picking a capture format, as decoding costs CPU time.
bool IsCompressedPixelFormat(uint32_t fourcc);

// "YUYV", "MJPG", ... Non printable characters are shown as '?'.
std::string FourccToString(uint32_t fourcc);

}  // namespace aidl::android::hardware::camera::mainline
