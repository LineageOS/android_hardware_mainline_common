/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Jpeg"

#include "jpeg/JpegEncoder.h"

// clang-format off
#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>
// clang-format on

#include <algorithm>
#include <array>

#include <android-base/logging.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

// Largest APP1 segment payload: 64 KiB minus the length field.
constexpr size_t kMaxApp1Size = 65533;

// libjpeg's default error handler exits the process; jump back instead.
struct ErrorManager {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void OnError(j_common_ptr cinfo) {
    char message[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, message);
    LOG(ERROR) << "libjpeg: " << message;
    longjmp(reinterpret_cast<ErrorManager*>(cinfo->err)->jump, 1);
}

void OnMessage(j_common_ptr cinfo) {
    char message[JMSG_LENGTH_MAX];
    (*cinfo->err->format_message)(cinfo, message);
    LOG(DEBUG) << "libjpeg: " << message;
}

// Writes straight into the output buffer. Data that does not fit goes into a
// spill area and marks the result as failed.
struct Destination {
    jpeg_destination_mgr pub;
    uint8_t* start;
    size_t capacity;
    bool overflow;
    JOCTET spill[256];
};

void InitDestination(j_compress_ptr cinfo) {
    auto* destination = reinterpret_cast<Destination*>(cinfo->dest);
    destination->pub.next_output_byte = destination->start;
    destination->pub.free_in_buffer = destination->capacity;
}

boolean EmptyOutputBuffer(j_compress_ptr cinfo) {
    auto* destination = reinterpret_cast<Destination*>(cinfo->dest);
    destination->overflow = true;
    destination->pub.next_output_byte = destination->spill;
    destination->pub.free_in_buffer = sizeof(destination->spill);
    return TRUE;
}

void TermDestination(j_compress_ptr /*cinfo*/) {}

}  // namespace

size_t EncodeJpeg(const I420Image& image, int quality, const std::vector<uint8_t>& app1,
                  uint8_t* output, size_t capacity) {
    if (app1.size() > kMaxApp1Size) {
        LOG(ERROR) << "APP1 segment too large: " << app1.size();
        return 0;
    }
    const int width = image.width();
    const int height = image.height();

    // Only trivially destructible locals from here on: libjpeg errors
    // longjmp() back to the setjmp() below.
    jpeg_compress_struct cinfo;
    ErrorManager error;
    Destination destination;
    JSAMPROW y_rows[16];
    JSAMPROW cb_rows[8];
    JSAMPROW cr_rows[8];
    JSAMPARRAY planes[3] = {y_rows, cb_rows, cr_rows};

    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = OnError;
    error.pub.output_message = OnMessage;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&cinfo);
        return 0;
    }
    jpeg_create_compress(&cinfo);

    destination.pub.init_destination = InitDestination;
    destination.pub.empty_output_buffer = EmptyOutputBuffer;
    destination.pub.term_destination = TermDestination;
    destination.start = output;
    destination.capacity = capacity;
    destination.overflow = false;
    cinfo.dest = &destination.pub;

    cinfo.image_width = static_cast<JDIMENSION>(width);
    cinfo.image_height = static_cast<JDIMENSION>(height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
    // Hand over subsampled planes as they are.
    cinfo.raw_data_in = TRUE;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = 1;
    cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = 1;
    cinfo.comp_info[2].v_samp_factor = 1;

    jpeg_start_compress(&cinfo, TRUE);
    if (!app1.empty()) {
        jpeg_write_marker(&cinfo, JPEG_APP0 + 1, app1.data(), static_cast<unsigned>(app1.size()));
    }

    const int chroma_height = (height + 1) / 2;
    auto* y = const_cast<uint8_t*>(image.y());
    auto* u = const_cast<uint8_t*>(image.u());
    auto* v = const_cast<uint8_t*>(image.v());
    while (cinfo.next_scanline < cinfo.image_height) {
        // One MCU row: 16 luma and 8 chroma rows, repeating the last row
        // past the bottom.
        const int first = static_cast<int>(cinfo.next_scanline);
        for (int i = 0; i < 16; ++i) {
            const int row = std::min(first + i, height - 1);
            y_rows[i] = y + static_cast<size_t>(row) * image.y_stride();
        }
        for (int i = 0; i < 8; ++i) {
            const int row = std::min(first / 2 + i, chroma_height - 1);
            cb_rows[i] = u + static_cast<size_t>(row) * image.uv_stride();
            cr_rows[i] = v + static_cast<size_t>(row) * image.uv_stride();
        }
        jpeg_write_raw_data(&cinfo, planes, 16);
    }
    jpeg_finish_compress(&cinfo);

    const size_t size = capacity - destination.pub.free_in_buffer;
    const bool overflow = destination.overflow;
    jpeg_destroy_compress(&cinfo);
    if (overflow) {
        LOG(ERROR) << "JPEG of " << width << "x" << height << " does not fit into " << capacity
                   << " bytes";
        return 0;
    }
    return size;
}

void ToFullRange(I420Image* image) {
    if (image->full_range) return;
    std::array<uint8_t, 256> luma;
    std::array<uint8_t, 256> chroma;
    for (int i = 0; i < 256; ++i) {
        luma[i] = static_cast<uint8_t>(std::clamp((i - 16) * 255 / 219, 0, 255));
        chroma[i] = static_cast<uint8_t>(std::clamp((i - 128) * 255 / 224 + 128, 0, 255));
    }
    const size_t luma_size = static_cast<size_t>(image->y_stride()) * image->height();
    const size_t chroma_size =
            static_cast<size_t>(image->uv_stride()) * ((image->height() + 1) / 2);
    std::transform(image->y(), image->y() + luma_size, image->y(),
                   [&](uint8_t c) { return luma[c]; });
    std::transform(image->u(), image->u() + chroma_size, image->u(),
                   [&](uint8_t c) { return chroma[c]; });
    std::transform(image->v(), image->v() + chroma_size, image->v(),
                   [&](uint8_t c) { return chroma[c]; });
    image->full_range = true;
}

}  // namespace aidl::android::hardware::camera::mainline
