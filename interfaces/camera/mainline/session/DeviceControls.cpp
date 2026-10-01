/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Controls"

#include "session/DeviceControls.h"

#include <linux/videodev2.h>

#include <android-base/logging.h>

namespace aidl::android::hardware::camera::mainline {

DeviceControls::DeviceControls(ControlDevice* device) : device_(device) {}

void DeviceControls::Reset() {
    if (device_->HasControl(V4L2_CID_POWER_LINE_FREQUENCY)) {
        // Not every device offers "auto" in the menu; then keep its default.
        if (!device_->SetControl(V4L2_CID_POWER_LINE_FREQUENCY,
                                 V4L2_CID_POWER_LINE_FREQUENCY_AUTO)) {
            LOG(DEBUG) << "no automatic power line frequency";
        }
    }
    if (device_->HasControl(V4L2_CID_3A_LOCK)) device_->SetControl(V4L2_CID_3A_LOCK, 0);
    constant_frame_rate_.reset();
    ae_locked_ = false;
    awb_locked_ = false;
    auto_exposure_mode_.reset();
}

void DeviceControls::SetConstantFrameRate(bool constant) {
    if (constant_frame_rate_ == constant) return;
    constant_frame_rate_ = constant;
    if (device_->HasControl(V4L2_CID_EXPOSURE_AUTO_PRIORITY)) {
        device_->SetControl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, constant ? 0 : 1);
    }
}

void DeviceControls::SetAeLock(bool lock) {
    if (ae_locked_ == lock) return;
    ae_locked_ = lock;

    if (device_->HasControl(V4L2_CID_3A_LOCK)) {
        const int32_t current = device_->GetControl(V4L2_CID_3A_LOCK).value_or(0);
        device_->SetControl(V4L2_CID_3A_LOCK, lock ? (current | V4L2_LOCK_EXPOSURE)
                                                   : (current & ~V4L2_LOCK_EXPOSURE));
        return;
    }
    if (!device_->HasControl(V4L2_CID_EXPOSURE_AUTO)) return;

    if (lock) {
        const auto mode = device_->GetControl(V4L2_CID_EXPOSURE_AUTO);
        if (!mode.has_value() || *mode == V4L2_EXPOSURE_MANUAL) return;
        // Freeze the exposure the automatic mode arrived at.
        const auto exposure = device_->GetControl(V4L2_CID_EXPOSURE_ABSOLUTE);
        auto_exposure_mode_ = mode;
        device_->SetControl(V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
        if (exposure.has_value()) device_->SetControl(V4L2_CID_EXPOSURE_ABSOLUTE, *exposure);
    } else if (auto_exposure_mode_.has_value()) {
        device_->SetControl(V4L2_CID_EXPOSURE_AUTO, *auto_exposure_mode_);
        auto_exposure_mode_.reset();
    }
}

void DeviceControls::SetAwbLock(bool lock) {
    if (awb_locked_ == lock) return;
    awb_locked_ = lock;

    if (device_->HasControl(V4L2_CID_3A_LOCK)) {
        const int32_t current = device_->GetControl(V4L2_CID_3A_LOCK).value_or(0);
        device_->SetControl(V4L2_CID_3A_LOCK, lock ? (current | V4L2_LOCK_WHITE_BALANCE)
                                                   : (current & ~V4L2_LOCK_WHITE_BALANCE));
        return;
    }
    if (!device_->HasControl(V4L2_CID_AUTO_WHITE_BALANCE)) return;

    if (lock) {
        // Freeze the temperature the automatic mode arrived at.
        const auto temperature = device_->GetControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE);
        device_->SetControl(V4L2_CID_AUTO_WHITE_BALANCE, 0);
        if (temperature.has_value()) {
            device_->SetControl(V4L2_CID_WHITE_BALANCE_TEMPERATURE, *temperature);
        }
    } else {
        device_->SetControl(V4L2_CID_AUTO_WHITE_BALANCE, 1);
    }
}

}  // namespace aidl::android::hardware::camera::mainline
