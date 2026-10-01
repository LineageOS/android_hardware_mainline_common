/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Isp"

#include "isp/SoftIsp.h"

#include <algorithm>
#include <cmath>

#include <android-base/logging.h>
#include <libyuv/convert.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

// Statistics come from every kStatsStep-th 2x2 block in both directions.
constexpr uint32_t kStatsStep = 4;
// Samples at or above this fraction of the white level count as clipped.
constexpr double kClipped = 0.95;

double SrgbGamma(double linear) {
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

int Reflect(int i, int n) {
    return i < 0 ? -i : i >= n ? 2 * n - 2 - i : i;
}

}  // namespace

void SoftIsp::BuildLuts(const BayerFormat& bayer, const IspParams& params) {
    if (lut_params_ == params && lut_bits_ == bayer.bits) return;
    const int size = 1 << bayer.bits;
    const int white = size - 1;
    const int black = std::clamp(black_level_.value_or(16 << (bayer.bits - 8)), 0, white - 1);

    // The gamma curve, sampled finely enough for 12 bit input.
    static const std::vector<uint8_t> kGamma = [] {
        std::vector<uint8_t> table(4096);
        for (size_t i = 0; i < table.size(); ++i) {
            table[i] = static_cast<uint8_t>(
                    std::lround(255.0 * SrgbGamma(static_cast<double>(i) / (table.size() - 1))));
        }
        return table;
    }();

    const double gains[4] = {params.red_gain * params.digital_gain, params.digital_gain,
                             params.digital_gain, params.blue_gain * params.digital_gain};
    for (int color = 0; color < 4; ++color) {
        auto& lut = luts_[color];
        lut.resize(size);
        const double scale = gains[color] / (white - black);
        for (int value = 0; value < size; ++value) {
            const double linear = std::clamp((value - black) * scale, 0.0, 1.0);
            lut[value] = kGamma[static_cast<size_t>(linear * (kGamma.size() - 1) + 0.5)];
        }
    }
    lut_params_ = params;
    lut_bits_ = bayer.bits;
}

bool SoftIsp::Process(const CaptureFormat& format, const CapturedFrame& frame,
                      const IspParams& params, I420Image* out) {
    stats_ = {};
    const auto bayer = GetBayerFormat(format.fourcc);
    if (!bayer.has_value() || frame.error || frame.planes.size() != 1 ||
        format.planes.size() != 1) {
        return false;
    }
    // Whole 2x2 blocks only.
    const uint32_t width = format.width & ~1u;
    const uint32_t height = format.height & ~1u;
    if (width < 2 || height < 2) return false;
    const uint32_t row_bytes = bayer->RowBytes(width);
    const uint32_t stride = std::max(format.planes[0].bytes_per_line, row_bytes);
    const size_t needed = static_cast<size_t>(stride) * (height - 1) + row_bytes;
    if (frame.planes[0].bytes_used < needed) {
        LOG(DEBUG) << "dropping short raw frame " << frame.sequence << " ("
                   << frame.planes[0].bytes_used << " of " << needed << " bytes)";
        return false;
    }

    BuildLuts(*bayer, params);
    const int white = (1 << bayer->bits) - 1;
    const int black = std::clamp(black_level_.value_or(16 << (bayer->bits - 8)), 0, white - 1);
    const int clipped = static_cast<int>(white * kClipped);

    row_.resize(width);
    mosaic_.resize(static_cast<size_t>(width) * height);
    double sums[4] = {};
    uint64_t samples = 0;
    uint64_t clipped_samples = 0;
    for (uint32_t y = 0; y < height; ++y) {
        UnpackBayerRow(*bayer, frame.planes[0].data + static_cast<size_t>(y) * stride, width,
                       row_.data());
        uint8_t* dst = mosaic_.data() + static_cast<size_t>(y) * width;
        const auto& even = luts_[static_cast<int>(bayer->ColorAt(0, y))];
        const auto& odd = luts_[static_cast<int>(bayer->ColorAt(1, y))];
        for (uint32_t x = 0; x < width; x += 2) {
            dst[x] = even[row_[x]];
            dst[x + 1] = odd[row_[x + 1]];
        }

        if ((y / 2) % kStatsStep != 0) continue;
        for (uint32_t x = 0; x < width; x += 2 * kStatsStep) {
            for (uint32_t dx = 0; dx < 2; ++dx) {
                const int value = row_[x + dx];
                sums[static_cast<int>(bayer->ColorAt(x + dx, y))] += std::max(value - black, 0);
                clipped_samples += value >= clipped;
                ++samples;
            }
        }
    }
    if (samples > 0) {
        // A quarter of the samples are red, a quarter blue, half green.
        const double range = white - black;
        const double quarter = samples / 4.0;
        stats_.valid = true;
        stats_.red = sums[static_cast<int>(BayerColor::kRed)] / quarter / range;
        stats_.blue = sums[static_cast<int>(BayerColor::kBlue)] / quarter / range;
        stats_.green = (sums[static_cast<int>(BayerColor::kGreenRed)] +
                        sums[static_cast<int>(BayerColor::kGreenBlue)]) /
                       (2 * quarter) / range;
        stats_.saturated = static_cast<double>(clipped_samples) / samples;
    }

    argb_.resize(static_cast<size_t>(width) * height * 4);
    DemosaicBilinear(mosaic_.data(), static_cast<int>(width), static_cast<int>(height), *bayer,
                     argb_.data());

    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    out->Resize(w, h);
    out->full_range = false;
    if (libyuv::ARGBToI420(argb_.data(), w * 4, out->y(), out->y_stride(), out->u(),
                           out->uv_stride(), out->v(), out->uv_stride(), w, h) != 0) {
        return false;
    }

    static const std::array<uint8_t, 256> kChroma = [] {
        std::array<uint8_t, 256> table;
        for (int i = 0; i < 256; ++i) {
            table[i] = static_cast<uint8_t>(std::clamp(
                    static_cast<int>(std::lround(128 + (i - 128) * kSaturation)), 16, 240));
        }
        return table;
    }();
    const size_t chroma = static_cast<size_t>(out->uv_stride()) * ((h + 1) / 2);
    for (uint8_t* plane : {out->u(), out->v()}) {
        for (size_t i = 0; i < chroma; ++i) plane[i] = kChroma[plane[i]];
    }
    return true;
}

void DemosaicBilinear(const uint8_t* mosaic, int width, int height, const BayerFormat& format,
                      uint8_t* argb) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* up = mosaic + static_cast<size_t>(Reflect(y - 1, height)) * width;
        const uint8_t* mid = mosaic + static_cast<size_t>(y) * width;
        const uint8_t* down = mosaic + static_cast<size_t>(Reflect(y + 1, height)) * width;
        uint8_t* out = argb + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            const int left = Reflect(x - 1, width);
            const int right = Reflect(x + 1, width);
            const int center = mid[x];
            const int horizontal = (mid[left] + mid[right] + 1) >> 1;
            const int vertical = (up[x] + down[x] + 1) >> 1;
            int r, g, b;
            switch (format.ColorAt(x, y)) {
                case BayerColor::kRed:
                    r = center;
                    g = (mid[left] + mid[right] + up[x] + down[x] + 2) >> 2;
                    b = (up[left] + up[right] + down[left] + down[right] + 2) >> 2;
                    break;
                case BayerColor::kBlue:
                    b = center;
                    g = (mid[left] + mid[right] + up[x] + down[x] + 2) >> 2;
                    r = (up[left] + up[right] + down[left] + down[right] + 2) >> 2;
                    break;
                case BayerColor::kGreenRed:
                    // Red to the left and right, blue above and below.
                    g = center;
                    r = horizontal;
                    b = vertical;
                    break;
                case BayerColor::kGreenBlue:
                default:
                    g = center;
                    b = horizontal;
                    r = vertical;
                    break;
            }
            out[4 * x] = static_cast<uint8_t>(b);
            out[4 * x + 1] = static_cast<uint8_t>(g);
            out[4 * x + 2] = static_cast<uint8_t>(r);
            out[4 * x + 3] = 255;
        }
    }
}

}  // namespace aidl::android::hardware::camera::mainline
