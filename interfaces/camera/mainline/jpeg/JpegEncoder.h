/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "convert/Image.h"

namespace aidl::android::hardware::camera::mainline {

// Encodes `image` as a baseline 4:2:0 JPEG of `quality` (1..100) into
// `output` of `capacity` bytes, with `app1` (EXIF, without the marker) as its
// APP1 segment when not empty. The image has to be full range (JFIF).
// Returns the size of the JPEG, or 0 when it failed or did not fit.
size_t EncodeJpeg(const I420Image& image, int quality, const std::vector<uint8_t>& app1,
                  uint8_t* output, size_t capacity);

// Converts a video range image to the full range JPEG expects, in place.
void ToFullRange(I420Image* image);

}  // namespace aidl::android::hardware::camera::mainline
