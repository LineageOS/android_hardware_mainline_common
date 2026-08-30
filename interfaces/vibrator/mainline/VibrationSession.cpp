/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineVibratorVibrationSession"

#include "vibrator-impl/VibrationSession.h"

#include <android-base/logging.h>
#include <thread>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static constexpr int32_t kSessionEndDelayMs = 20;

ndk::ScopedAStatus VibrationSession::close() {
    LOG(VERBOSE) << "VibrationSession: close";
    mManager->closeSession(kSessionEndDelayMs);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibrationSession::abort() {
    LOG(VERBOSE) << "VibrationSession: abort";
    mManager->abortSession();
    return ndk::ScopedAStatus::ok();
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
