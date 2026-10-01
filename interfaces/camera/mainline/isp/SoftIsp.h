/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "convert/Image.h"
#include "isp/BayerFormat.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// Statistics of a raw frame, gathered on a grid of 2x2 blocks: mean linear
// values (0-1, black level removed, before any gain) per colour, and the
// fraction of clipped samples.
struct IspStats {
    bool valid = false;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    double saturated = 0.0;
};

// What 3A tells the ISP to apply.
struct IspParams {
    double red_gain = 1.0;
    double blue_gain = 1.0;
    double digital_gain = 1.0;

    bool operator==(const IspParams&) const = default;
};

// A minimal ISP on the CPU for sensors that only deliver raw Bayer data:
//   1. black level, white balance and digital gain, sRGB gamma: one lookup
//      table per Bayer colour, from the raw sample to 8 bit;
//   2. bilinear demosaicing to RGB;
//   3. RGB to I420 (BT.601), with a little extra saturation, as there is no
//      colour correction matrix.
// There is no lens shading correction, denoising or sharpening.
class SoftIsp {
  public:
    // Saturation applied to the chroma planes.
    static constexpr double kSaturation = 1.25;

    // `black_level` in the sensor's bit depth; nullopt: 16 at 8 bit (64 at
    // 10 bit), common to most sensors.
    explicit SoftIsp(std::optional<int> black_level = std::nullopt) : black_level_(black_level) {}

    // Processes one frame of `format` (a Bayer format, see IsIspPixelFormat())
    // into `out`, and gathers its statistics. False if the frame is unusable.
    bool Process(const CaptureFormat& format, const CapturedFrame& frame, const IspParams& params,
                 I420Image* out);

    const IspStats& stats() const { return stats_; }

  private:
    void BuildLuts(const BayerFormat& bayer, const IspParams& params);

    const std::optional<int> black_level_;
    IspStats stats_;

    // Lookup tables by BayerColor, and what they were built for.
    std::array<std::vector<uint8_t>, 4> luts_;
    std::optional<IspParams> lut_params_;
    int lut_bits_ = 0;

    std::vector<uint16_t> row_;
    // The frame after the lookup tables, one byte per sample.
    std::vector<uint8_t> mosaic_;
    // Demosaiced, libyuv ARGB (B, G, R, A in memory).
    std::vector<uint8_t> argb_;
};

// Demosaics an 8 bit Bayer mosaic of even width and height (at least 2x2)
// into libyuv ARGB. Exposed for the unit tests.
void DemosaicBilinear(const uint8_t* mosaic, int width, int height, const BayerFormat& format,
                      uint8_t* argb);

}  // namespace aidl::android::hardware::camera::mainline
