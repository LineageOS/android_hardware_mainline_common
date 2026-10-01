/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

#include "session/RequestSettings.h"

namespace aidl::android::hardware::camera::mainline {

// Decides when a session lights the flash, and the AE and flash states it
// reports. Without hardware strobe synchronization, "firing" the flash means
// lighting the LEDs at torch brightness and capturing a frame that was
// exposed after they came on:
//   FLASH_MODE TORCH  - lit while requested.
//   FLASH_MODE SINGLE - lit for the request's frame.
//   AE_MODE ON_ALWAYS_FLASH / ON_AUTO_FLASH (FLASH_MODE is ignored then) -
//       lit for still captures, always or when the scene is dark. An AE
//       precapture trigger lights them ahead (pre-flash) for a while, so
//       that the camera's auto exposure adapts before the still capture.
class FlashControl {
  public:
    // Pre-flash: time the AE precapture sequence takes, and how long the
    // flash stays lit afterwards waiting for the still capture.
    static constexpr int64_t kPrecaptureNs = 500'000'000;
    static constexpr int64_t kPreflashTimeoutNs = 3'000'000'000;
    // Time a frame has to start after the flash came on, when it is lit for
    // a capture without pre-flash.
    static constexpr int64_t kSettleNs = 150'000'000;
    // Mean luma (of 255) below which the scene is too dark without flash.
    static constexpr int kDarkLuma = 60;

    // What to do for a request.
    struct Plan {
        // Light the flash for the frame.
        bool lit = false;
        // Keep it lit after the frame (torch, pre-flash); otherwise it goes
        // off again.
        bool keep_lit = false;
        // CLOCK_MONOTONIC time the frame has to start after (the flash has to
        // be lit during it), 0 for any frame.
        int64_t not_before_ns = 0;
        // ANDROID_CONTROL_AE_STATE, ANDROID_FLASH_STATE.
        uint8_t ae_state = 0;
        uint8_t flash_state = 0;
    };

    // `available`: the camera has a flash. Without one, nothing is ever lit
    // and the states are the ones of a camera without flash.
    explicit FlashControl(bool available) : available_(available) {}

    // Before capturing the frame of a request; `now_ns` is CLOCK_MONOTONIC.
    Plan Begin(const RequestSettings& settings, int64_t now_ns);
    // The flash went off after a frame (Plan::keep_lit was false).
    void Unlit() { lit_ = false; }

    // Whether the brightness of the captured frame is needed (auto flash
    // with the flash off).
    bool WantsBrightness(const RequestSettings& settings) const;
    // Mean luma of a frame captured without flash.
    void SetBrightness(int mean_luma) { dark_ = mean_luma < kDarkLuma; }

  private:
    enum class Precapture { kIdle, kRunning, kDone };

    const bool available_;
    Precapture precapture_ = Precapture::kIdle;
    int64_t precapture_start_ns_ = 0;
    // The precapture sequence lit the flash.
    bool preflash_ = false;
    bool lit_ = false;
    int64_t lit_since_ns_ = 0;
    bool dark_ = false;
};

}  // namespace aidl::android::hardware::camera::mainline
