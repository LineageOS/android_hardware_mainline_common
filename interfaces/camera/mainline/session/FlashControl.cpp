/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "session/FlashControl.h"

#include <system/camera_metadata.h>

namespace aidl::android::hardware::camera::mainline {

FlashControl::Plan FlashControl::Begin(const RequestSettings& settings, int64_t now_ns) {
    Plan plan;
    if (!available_) {
        plan.ae_state = settings.ae_lock ? ANDROID_CONTROL_AE_STATE_LOCKED
                                         : ANDROID_CONTROL_AE_STATE_CONVERGED;
        plan.flash_state = ANDROID_FLASH_STATE_UNAVAILABLE;
        return plan;
    }

    const bool auto_flash = settings.ae_mode == ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH;
    const bool flash_ae = auto_flash || settings.ae_mode == ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
    const bool needs_flash = flash_ae && (!auto_flash || dark_);

    switch (settings.precapture_trigger) {
        case ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START:
            precapture_ = Precapture::kRunning;
            precapture_start_ns_ = now_ns;
            preflash_ = needs_flash;
            break;
        case ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL:
            precapture_ = Precapture::kIdle;
            preflash_ = false;
            break;
        default:
            break;
    }
    const int64_t since_trigger = now_ns - precapture_start_ns_;
    if (precapture_ == Precapture::kRunning && since_trigger >= kPrecaptureNs) {
        precapture_ = Precapture::kDone;
    }
    if (preflash_ && (!flash_ae || since_trigger >= kPrecaptureNs + kPreflashTimeoutNs)) {
        preflash_ = false;
    }

    bool lit = false;
    bool wait = false;
    if (flash_ae) {
        if (settings.still_capture && needs_flash) {
            lit = true;
            wait = true;
            // The capture the pre-flash was for.
            preflash_ = false;
            precapture_ = Precapture::kIdle;
        } else if (preflash_) {
            lit = true;
            plan.keep_lit = true;
        }
    } else if (settings.flash_mode == ANDROID_FLASH_MODE_TORCH) {
        lit = true;
        plan.keep_lit = true;
    } else if (settings.flash_mode == ANDROID_FLASH_MODE_SINGLE) {
        lit = true;
        wait = true;
    }

    if (lit && !lit_) lit_since_ns_ = now_ns;
    lit_ = lit;
    plan.lit = lit;
    plan.not_before_ns = wait ? lit_since_ns_ + kSettleNs : 0;
    plan.flash_state = lit ? ANDROID_FLASH_STATE_FIRED : ANDROID_FLASH_STATE_READY;
    if (settings.ae_lock) {
        plan.ae_state = ANDROID_CONTROL_AE_STATE_LOCKED;
    } else if (precapture_ == Precapture::kRunning) {
        plan.ae_state = ANDROID_CONTROL_AE_STATE_PRECAPTURE;
    } else if (auto_flash && dark_ && !lit) {
        plan.ae_state = ANDROID_CONTROL_AE_STATE_FLASH_REQUIRED;
    } else {
        plan.ae_state = ANDROID_CONTROL_AE_STATE_CONVERGED;
    }
    return plan;
}

bool FlashControl::WantsBrightness(const RequestSettings& settings) const {
    return available_ && !lit_ && settings.ae_mode == ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH;
}

}  // namespace aidl::android::hardware::camera::mainline
