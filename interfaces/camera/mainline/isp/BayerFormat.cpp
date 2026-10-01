/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "isp/BayerFormat.h"

#include <linux/videodev2.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

constexpr BayerColor R = BayerColor::kRed;
constexpr BayerColor Gr = BayerColor::kGreenRed;
constexpr BayerColor Gb = BayerColor::kGreenBlue;
constexpr BayerColor B = BayerColor::kBlue;

BayerFormat Make(char order, int bits, BayerPacking packing) {
    // Green on a red row is Gr, on a blue row Gb.
    switch (order) {
        case 'b':  // BGGR
            return {{B, Gb, Gr, R}, bits, packing};
        case 'g':  // GBRG
            return {{Gb, B, R, Gr}, bits, packing};
        case 'G':  // GRBG
            return {{Gr, R, B, Gb}, bits, packing};
        default:  // RGGB
            return {{R, Gr, Gb, B}, bits, packing};
    }
}

}  // namespace

uint32_t BayerFormat::RowBytes(uint32_t width) const {
    switch (packing) {
        case BayerPacking::k8:
            return width;
        case BayerPacking::k16:
            return width * 2;
        case BayerPacking::kMipi10:
            return (width * 5 + 3) / 4;
        case BayerPacking::kMipi12:
            return (width * 3 + 1) / 2;
    }
    return 0;
}

std::optional<BayerFormat> GetBayerFormat(uint32_t fourcc) {
    switch (fourcc) {
        case V4L2_PIX_FMT_SBGGR8:
            return Make('b', 8, BayerPacking::k8);
        case V4L2_PIX_FMT_SGBRG8:
            return Make('g', 8, BayerPacking::k8);
        case V4L2_PIX_FMT_SGRBG8:
            return Make('G', 8, BayerPacking::k8);
        case V4L2_PIX_FMT_SRGGB8:
            return Make('r', 8, BayerPacking::k8);
        case V4L2_PIX_FMT_SBGGR10:
            return Make('b', 10, BayerPacking::k16);
        case V4L2_PIX_FMT_SGBRG10:
            return Make('g', 10, BayerPacking::k16);
        case V4L2_PIX_FMT_SGRBG10:
            return Make('G', 10, BayerPacking::k16);
        case V4L2_PIX_FMT_SRGGB10:
            return Make('r', 10, BayerPacking::k16);
        case V4L2_PIX_FMT_SBGGR10P:
            return Make('b', 10, BayerPacking::kMipi10);
        case V4L2_PIX_FMT_SGBRG10P:
            return Make('g', 10, BayerPacking::kMipi10);
        case V4L2_PIX_FMT_SGRBG10P:
            return Make('G', 10, BayerPacking::kMipi10);
        case V4L2_PIX_FMT_SRGGB10P:
            return Make('r', 10, BayerPacking::kMipi10);
        case V4L2_PIX_FMT_SBGGR12:
            return Make('b', 12, BayerPacking::k16);
        case V4L2_PIX_FMT_SGBRG12:
            return Make('g', 12, BayerPacking::k16);
        case V4L2_PIX_FMT_SGRBG12:
            return Make('G', 12, BayerPacking::k16);
        case V4L2_PIX_FMT_SRGGB12:
            return Make('r', 12, BayerPacking::k16);
        case V4L2_PIX_FMT_SBGGR12P:
            return Make('b', 12, BayerPacking::kMipi12);
        case V4L2_PIX_FMT_SGBRG12P:
            return Make('g', 12, BayerPacking::kMipi12);
        case V4L2_PIX_FMT_SGRBG12P:
            return Make('G', 12, BayerPacking::kMipi12);
        case V4L2_PIX_FMT_SRGGB12P:
            return Make('r', 12, BayerPacking::kMipi12);
        default:
            return std::nullopt;
    }
}

void UnpackBayerRow(const BayerFormat& format, const uint8_t* src, uint32_t width, uint16_t* dst) {
    switch (format.packing) {
        case BayerPacking::k8:
            for (uint32_t x = 0; x < width; ++x) dst[x] = src[x];
            break;
        case BayerPacking::k16: {
            const uint16_t mask = static_cast<uint16_t>((1u << format.bits) - 1);
            for (uint32_t x = 0; x < width; ++x) {
                dst[x] = static_cast<uint16_t>((src[2 * x] | (src[2 * x + 1] << 8)) & mask);
            }
            break;
        }
        case BayerPacking::kMipi10:
            // 4 samples: their high 8 bits, then one byte with the low 2 bits
            // of each (first sample in the lowest bits).
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* group = src + (x / 4) * 5;
                const uint32_t i = x % 4;
                dst[x] = static_cast<uint16_t>((group[i] << 2) | ((group[4] >> (2 * i)) & 3));
            }
            break;
        case BayerPacking::kMipi12:
            // 2 samples: their high 8 bits, then one byte with both low nibbles.
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t* group = src + (x / 2) * 3;
                const uint32_t i = x % 2;
                dst[x] = static_cast<uint16_t>((group[i] << 4) | ((group[2] >> (4 * i)) & 0xf));
            }
            break;
    }
}

}  // namespace aidl::android::hardware::camera::mainline
