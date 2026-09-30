/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "convert/Image.h"
#include "utils/Metadata.h"

namespace aidl::android::hardware::camera::mainline {

// Intermediate images and buffers, reused from capture to capture.
struct JpegWorkspace {
    I420Image image;
    I420Image thumbnail;
    std::vector<uint8_t> thumbnail_jpeg;
    std::vector<uint8_t> app1;
};

// What goes into the EXIF data besides the request settings.
struct JpegContext {
    // Static characteristics (flash availability, ...).
    const Metadata* characteristics = nullptr;
    std::string make;
    std::string model;
};

// Encodes the `region` of `source`, center cropped to the aspect ratio of
// `size`, as a JPEG of `size` with EXIF data and thumbnail as the request
// `settings` ask for, into a BLOB buffer of `buffer_size` bytes. The buffer
// ends with the camera blob trailer the framework looks for.
bool WriteJpeg(const I420Image& source, const Rect& region, Size size, const Metadata& settings,
               const JpegContext& context, uint8_t* buffer, size_t buffer_size,
               JpegWorkspace* workspace);

}  // namespace aidl::android::hardware::camera::mainline
