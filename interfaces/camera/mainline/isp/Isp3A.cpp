/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Isp"

#include "isp/Isp3A.h"

#include <linux/videodev2.h>

#include <algorithm>
#include <cmath>

#include <android-base/logging.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

// Share of the way to the target taken per update.
constexpr double kAwbSpeed = 0.3;
// Exponent applied to the AE correction, < 1 to approach smoothly.
constexpr double kAeDamping = 0.7;
// Largest correction per AE update.
constexpr double kMaxAeStep = 4.0;
// More clipped samples than this means overexposed, whatever the mean.
constexpr double kMaxSaturated = 0.05;

}  // namespace

void Isp3A::Start() {
    settle_frames_ = 0;
    exposure_range_.reset();
    gain_range_.reset();
    if (sensor_ == nullptr) return;

    exposure_range_ = sensor_->GetControlRange(V4L2_CID_EXPOSURE);
    const auto exposure = sensor_->GetControl(V4L2_CID_EXPOSURE);
    if (!exposure_range_.has_value() || !exposure.has_value()) {
        exposure_range_.reset();
    } else {
        exposure_ = *exposure;
    }
    for (const uint32_t id : {V4L2_CID_ANALOGUE_GAIN, V4L2_CID_GAIN}) {
        const auto range = sensor_->GetControlRange(id);
        const auto gain = sensor_->GetControl(id);
        if (range.has_value() && gain.has_value() && range->minimum > 0) {
            gain_id_ = id;
            gain_range_ = range;
            gain_ = *gain;
            break;
        }
    }
    LOG(DEBUG) << "AE: exposure "
               << (exposure_range_ ? std::to_string(exposure_) : std::string("n/a")) << ", gain "
               << (gain_range_ ? std::to_string(gain_) : std::string("n/a"));
}

void Isp3A::SetLocks(bool ae, bool awb) {
    ae_lock_ = ae;
    awb_lock_ = awb;
}

void Isp3A::Update(const IspStats& stats) {
    if (!stats.valid) return;

    constexpr double kTiny = 1e-4;
    if (!awb_lock_ && stats.red > kTiny && stats.blue > kTiny && stats.green > kTiny) {
        const double red =
                std::clamp(stats.green / stats.red, kMinWhiteBalanceGain, kMaxWhiteBalanceGain);
        const double blue =
                std::clamp(stats.green / stats.blue, kMinWhiteBalanceGain, kMaxWhiteBalanceGain);
        // The first estimate is taken as it is.
        const double speed = awb_started_ ? kAwbSpeed : 1.0;
        params_.red_gain += (red - params_.red_gain) * speed;
        params_.blue_gain += (blue - params_.blue_gain) * speed;
        awb_started_ = true;
    }

    if (ae_lock_) return;
    if (settle_frames_ > 0) {
        --settle_frames_;
        return;
    }
    // Luminance (BT.601 weights) after white balance and digital gain.
    const double luminance = (0.299 * stats.red * params_.red_gain + 0.587 * stats.green +
                              0.114 * stats.blue * params_.blue_gain) *
                             params_.digital_gain;
    double ratio = kTargetLuminance / std::max(luminance, kTiny);
    if (stats.saturated > kMaxSaturated) ratio = std::min(ratio, 0.7);
    if (ratio < kTolerance && ratio > 1.0 / kTolerance) return;
    ratio = std::clamp(std::pow(ratio, kAeDamping), 1.0 / kMaxAeStep, kMaxAeStep);
    Expose(ratio);
}

void Isp3A::Expose(double ratio) {
    // Everything relative to the minimum gain (1x).
    const double exposure = exposure_range_ ? std::max(exposure_, 1) : 1.0;
    const double gain = gain_range_ ? static_cast<double>(gain_) / gain_range_->minimum : 1.0;
    double target = exposure * gain * params_.digital_gain * ratio;

    int32_t new_exposure = exposure_;
    if (exposure_range_) {
        // The range follows the frame length, which may have changed.
        if (auto range = sensor_->GetControlRange(V4L2_CID_EXPOSURE)) exposure_range_ = range;
        new_exposure = static_cast<int32_t>(std::clamp(
                std::lround(target), static_cast<long>(std::max(exposure_range_->minimum, 1)),
                static_cast<long>(exposure_range_->maximum)));
        target /= new_exposure;
    }
    int32_t new_gain = gain_;
    if (gain_range_) {
        const double factor = std::clamp(
                target, 1.0, static_cast<double>(gain_range_->maximum) / gain_range_->minimum);
        const int32_t step = std::max(gain_range_->step, 1);
        new_gain = gain_range_->minimum +
                   static_cast<int32_t>(std::lround((factor - 1.0) * gain_range_->minimum / step)) *
                           step;
        new_gain = std::clamp(new_gain, gain_range_->minimum, gain_range_->maximum);
        target /= static_cast<double>(new_gain) / gain_range_->minimum;
    }
    // Without sensor controls digital gain may also go below 1.
    const double min_digital = exposure_range_ || gain_range_ ? 1.0 : 1.0 / kMaxDigitalGain;
    params_.digital_gain = std::clamp(target, min_digital, kMaxDigitalGain);

    bool changed = false;
    if (exposure_range_ && new_exposure != exposure_) {
        if (sensor_->SetControl(V4L2_CID_EXPOSURE, new_exposure)) {
            exposure_ = new_exposure;
            changed = true;
        }
    }
    if (gain_range_ && new_gain != gain_) {
        if (sensor_->SetControl(gain_id_, new_gain)) {
            gain_ = new_gain;
            changed = true;
        }
    }
    if (changed) settle_frames_ = kSettleFrames;
}

}  // namespace aidl::android::hardware::camera::mainline
