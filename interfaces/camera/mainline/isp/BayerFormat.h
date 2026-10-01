/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>

namespace aidl::android::hardware::camera::mainline {

// Colour of a pixel of the Bayer pattern.
enum class BayerColor : uint8_t { kRed, kGreenRed, kGreenBlue, kBlue };

// How the samples of a raw Bayer pixel format are stored.
enum class BayerPacking {
    // One byte per sample.
    k8,
    // Two bytes per sample, little endian, in the low bits.
    k16,
    // MIPI CSI-2 packing: 4 samples in 5 bytes (10 bit), 2 in 3 (12 bit).
    kMipi10,
    kMipi12,
};

// A raw Bayer pixel format the software ISP reads.
struct BayerFormat {
    // Colours of the top left 2x2 block: (0, 0), (1, 0), (0, 1), (1, 1).
    BayerColor pattern[4];
    int bits = 0;
    BayerPacking packing = BayerPacking::k8;

    BayerColor ColorAt(uint32_t x, uint32_t y) const { return pattern[(y & 1) * 2 + (x & 1)]; }
    // Bytes of a row of `width` samples, without padding.
    uint32_t RowBytes(uint32_t width) const;
};

// The format of a V4L2 Bayer pixel format, nullopt when the software ISP can
// not read it (e.g. DPCM / A-law compressed or 14 bit packed).
std::optional<BayerFormat> GetBayerFormat(uint32_t fourcc);

inline bool IsIspPixelFormat(uint32_t fourcc) {
    return GetBayerFormat(fourcc).has_value();
}

// Unpacks a row of `width` samples.
void UnpackBayerRow(const BayerFormat& format, const uint8_t* src, uint32_t width, uint16_t* dst);

}  // namespace aidl::android::hardware::camera::mainline
