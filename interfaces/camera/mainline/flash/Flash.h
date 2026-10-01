/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/common/TorchModeStatus.h>

#include "flash/FlashLed.h"

namespace aidl::android::hardware::camera::mainline {

// The flash of a camera: one or more LEDs (e.g. a dual tone flash) switched
// together. Works as torch while the camera is closed; a session that has it
// acquired uses it as flash.
//
// Torch strength levels are those of the LED with the most; the others are
// scaled along.
class Flash {
  public:
    using TorchListener =
            std::function<void(::aidl::android::hardware::camera::common::TorchModeStatus)>;

    // Opens the LEDs by name (see OpenFlashLed()). LEDs that fail to open are
    // logged and left out; returns null if none is left.
    static std::shared_ptr<Flash> Open(const std::vector<std::string>& names,
                                       const std::string& leds_dir = kLedClassDir);

    explicit Flash(std::vector<std::unique_ptr<FlashLed>> leds);
    ~Flash();

    int32_t max_level() const { return max_level_; }
    // Torch strength of setTorchMode(true): the full torch brightness, which
    // drivers already limit to what the LED takes continuously.
    int32_t default_level() const { return max_level_; }

    // Torch status changes are reported here (called with the flash's lock
    // held; do not call back into the flash).
    void SetTorchListener(TorchListener listener);

    // ICameraDevice torch calls; CAMERA_IN_USE while a session has the flash
    // acquired. Turning the torch off resets its level to the default.
    ::aidl::android::hardware::camera::common::Status SetTorch(bool on);
    ::aidl::android::hardware::camera::common::Status SetTorchLevel(int32_t level);
    int32_t torch_level();

    // A session takes the flash over: the torch goes off and is unavailable
    // until Release(). Both are idempotent.
    void Acquire();
    void Release();

    // For the session that acquired it: lights the LEDs at full torch
    // brightness, or turns them off.
    void SetLit(bool lit);

    // The camera went away: turns the LEDs off and stops reporting.
    void Detach();

  private:
    void ApplyLocked(int32_t level);
    void NotifyLocked(::aidl::android::hardware::camera::common::TorchModeStatus status);

    const std::vector<std::unique_ptr<FlashLed>> leds_;
    const int32_t max_level_;

    std::mutex lock_;
    TorchListener listener_;
    bool acquired_ = false;
    bool torch_on_ = false;
    int32_t torch_level_;
};

}  // namespace aidl::android::hardware::camera::mainline
