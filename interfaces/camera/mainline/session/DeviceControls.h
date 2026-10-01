/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <optional>

#include "v4l2/Controls.h"

namespace aidl::android::hardware::camera::mainline {

// Maps the few 3A controls Android asks for onto the V4L2 camera controls a
// device happens to have. Everything is best effort: a device without a
// control keeps doing whatever it does.
class DeviceControls {
  public:
    // `device` is the video node, or the sensor of a media controller
    // pipeline.
    explicit DeviceControls(ControlDevice* device);

    // Defaults when a session starts: automatic power line frequency
    // (antibanding), 3A unlocked.
    void Reset();

    // Constant frame rate (fixed AE fps range): do not let auto exposure
    // lower the frame rate in low light.
    void SetConstantFrameRate(bool constant);
    void SetAeLock(bool lock);
    void SetAwbLock(bool lock);

  private:
    ControlDevice* const device_;
    std::optional<bool> constant_frame_rate_;
    bool ae_locked_ = false;
    bool awb_locked_ = false;
    // Auto exposure mode to go back to after an AE lock without
    // V4L2_CID_3A_LOCK.
    std::optional<int32_t> auto_exposure_mode_;
};

}  // namespace aidl::android::hardware::camera::mainline
