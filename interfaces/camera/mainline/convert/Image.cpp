/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "convert/Image.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

// Readable bytes past the last plane: the JPEG encoder reads whole 16 pixel
// blocks, past the end of rows whose width is no multiple of 16.
constexpr size_t kSlack = 64;

}  // namespace

void I420Image::Resize(int32_t width, int32_t height) {
    width_ = width;
    height_ = height;
    const size_t needed = y_size() + 2 * uv_size() + kSlack;
    if (data_.size() < needed) data_.resize(needed);
}

}  // namespace aidl::android::hardware::camera::mainline
