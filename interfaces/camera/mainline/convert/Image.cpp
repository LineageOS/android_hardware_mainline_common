/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "convert/Image.h"

namespace aidl::android::hardware::camera::mainline {

void I420Image::Resize(int32_t width, int32_t height) {
    width_ = width;
    height_ = height;
    const size_t needed = y_size() + 2 * uv_size();
    if (data_.size() < needed) data_.resize(needed);
}

}  // namespace aidl::android::hardware::camera::mainline
