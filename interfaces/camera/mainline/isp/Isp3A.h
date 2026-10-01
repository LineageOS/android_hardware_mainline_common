/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>

#include "isp/SoftIsp.h"
#include "v4l2/Controls.h"

namespace aidl::android::hardware::camera::mainline {

// Auto exposure and white balance for the software ISP, from its statistics:
//   AWB: gray world, i.e. gains that make the mean red and blue match green.
//   AE:  brings the mean luminance to mid gray, with the sensor's exposure
//        time first (V4L2_CID_EXPOSURE, within the current frame length, so
//        the frame rate stays), then its analogue gain (V4L2_CID_ANALOGUE_GAIN
//        or V4L2_CID_GAIN), then the ISP's digital gain.
// Changes are damped, and AE waits for a change to show in the frames
// (sensors apply them a frame or two late) before it measures again.
class Isp3A {
  public:
    // Mean linear luminance AE aims for: 18% gray.
    static constexpr double kTargetLuminance = 0.18;
    // Deviation AE tolerates before it changes anything (as a ratio).
    static constexpr double kTolerance = 1.15;
    // Frames AE skips after a change.
    static constexpr int kSettleFrames = 2;
    static constexpr double kMaxDigitalGain = 4.0;
    static constexpr double kMinWhiteBalanceGain = 0.25;
    static constexpr double kMaxWhiteBalanceGain = 4.0;

    // `sensor` gets the exposure / gain controls; may be null, then only
    // digital gain is used.
    explicit Isp3A(ControlDevice* sensor) : sensor_(sensor) {}

    // When streaming starts: reads the sensor's current exposure and gain,
    // and their ranges.
    void Start();
    void SetLocks(bool ae, bool awb);

    // Feeds the statistics of a frame.
    void Update(const IspStats& stats);

    const IspParams& params() const { return params_; }

  private:
    // Applies a new total exposure (exposure lines x analogue gain factor x
    // digital gain, relative to the current one).
    void Expose(double ratio);

    ControlDevice* const sensor_;
    IspParams params_;
    bool ae_lock_ = false;
    bool awb_lock_ = false;
    bool awb_started_ = false;
    int settle_frames_ = 0;

    // Sensor controls, when it has them.
    std::optional<ControlRange> exposure_range_;
    int32_t exposure_ = 0;
    uint32_t gain_id_ = 0;
    std::optional<ControlRange> gain_range_;
    int32_t gain_ = 0;
};

}  // namespace aidl::android::hardware::camera::mainline
