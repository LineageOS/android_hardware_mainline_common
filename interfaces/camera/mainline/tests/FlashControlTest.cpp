/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <system/camera_metadata.h>

#include "session/FlashControl.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

constexpr int64_t kFrameNs = 33'000'000;

RequestSettings Settings(uint8_t ae_mode, uint8_t flash_mode = ANDROID_FLASH_MODE_OFF) {
    RequestSettings settings;
    settings.ae_mode = ae_mode;
    settings.flash_mode = flash_mode;
    settings.precapture_trigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    return settings;
}

RequestSettings Still(uint8_t ae_mode) {
    RequestSettings settings = Settings(ae_mode);
    settings.still_capture = true;
    return settings;
}

RequestSettings Trigger(uint8_t ae_mode, uint8_t trigger) {
    RequestSettings settings = Settings(ae_mode);
    settings.precapture_trigger = trigger;
    return settings;
}

TEST(FlashControlTest, NoFlash) {
    FlashControl control(false);
    auto plan = control.Begin(Settings(ANDROID_CONTROL_AE_MODE_ON, ANDROID_FLASH_MODE_TORCH), 0);
    EXPECT_FALSE(plan.lit);
    EXPECT_EQ(plan.flash_state, ANDROID_FLASH_STATE_UNAVAILABLE);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_CONVERGED);

    RequestSettings locked =
            Trigger(ANDROID_CONTROL_AE_MODE_ON, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START);
    locked.ae_lock = true;
    plan = control.Begin(locked, kFrameNs);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_LOCKED);
    EXPECT_FALSE(control.WantsBrightness(Settings(ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH)));
}

TEST(FlashControlTest, Torch) {
    FlashControl control(true);
    auto plan = control.Begin(Settings(ANDROID_CONTROL_AE_MODE_ON), 0);
    EXPECT_FALSE(plan.lit);
    EXPECT_EQ(plan.flash_state, ANDROID_FLASH_STATE_READY);

    plan = control.Begin(Settings(ANDROID_CONTROL_AE_MODE_ON, ANDROID_FLASH_MODE_TORCH), kFrameNs);
    EXPECT_TRUE(plan.lit);
    EXPECT_TRUE(plan.keep_lit);
    EXPECT_EQ(plan.not_before_ns, 0);
    EXPECT_EQ(plan.flash_state, ANDROID_FLASH_STATE_FIRED);

    plan = control.Begin(Settings(ANDROID_CONTROL_AE_MODE_ON), 2 * kFrameNs);
    EXPECT_FALSE(plan.lit);
    EXPECT_EQ(plan.flash_state, ANDROID_FLASH_STATE_READY);
}

TEST(FlashControlTest, Single) {
    FlashControl control(true);
    const int64_t now = 10 * kFrameNs;
    auto plan = control.Begin(Settings(ANDROID_CONTROL_AE_MODE_ON, ANDROID_FLASH_MODE_SINGLE), now);
    EXPECT_TRUE(plan.lit);
    EXPECT_FALSE(plan.keep_lit);
    EXPECT_EQ(plan.not_before_ns, now + FlashControl::kSettleNs);
    EXPECT_EQ(plan.flash_state, ANDROID_FLASH_STATE_FIRED);
    control.Unlit();

    plan = control.Begin(Settings(ANDROID_CONTROL_AE_MODE_ON), now + kFrameNs);
    EXPECT_FALSE(plan.lit);
}

TEST(FlashControlTest, AlwaysFlashWithPrecapture) {
    FlashControl control(true);
    const uint8_t mode = ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
    int64_t now = kFrameNs;

    // Preview: the flash mode is ignored with a flash AE mode.
    RequestSettings preview = Settings(mode, ANDROID_FLASH_MODE_TORCH);
    auto plan = control.Begin(preview, now);
    EXPECT_FALSE(plan.lit);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_CONVERGED);

    // Pre-flash.
    now += kFrameNs;
    plan = control.Begin(Trigger(mode, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START), now);
    EXPECT_TRUE(plan.lit);
    EXPECT_TRUE(plan.keep_lit);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_PRECAPTURE);
    const int64_t lit_at = now;

    now += FlashControl::kPrecaptureNs;
    plan = control.Begin(preview, now);
    EXPECT_TRUE(plan.lit);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_CONVERGED);

    // The still capture takes a frame lit since the pre-flash.
    now += kFrameNs;
    plan = control.Begin(Still(mode), now);
    EXPECT_TRUE(plan.lit);
    EXPECT_FALSE(plan.keep_lit);
    EXPECT_EQ(plan.not_before_ns, lit_at + FlashControl::kSettleNs);
    EXPECT_EQ(plan.flash_state, ANDROID_FLASH_STATE_FIRED);
    control.Unlit();

    now += kFrameNs;
    plan = control.Begin(preview, now);
    EXPECT_FALSE(plan.lit);
}

TEST(FlashControlTest, PreflashTimesOut) {
    FlashControl control(true);
    const uint8_t mode = ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
    auto plan = control.Begin(Trigger(mode, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START), 0);
    EXPECT_TRUE(plan.lit);
    plan = control.Begin(Settings(mode),
                         FlashControl::kPrecaptureNs + FlashControl::kPreflashTimeoutNs);
    EXPECT_FALSE(plan.lit);
}

TEST(FlashControlTest, PrecaptureCancel) {
    FlashControl control(true);
    const uint8_t mode = ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH;
    auto plan = control.Begin(Trigger(mode, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START), 0);
    EXPECT_TRUE(plan.lit);
    plan = control.Begin(Trigger(mode, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL), kFrameNs);
    EXPECT_FALSE(plan.lit);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_CONVERGED);
}

TEST(FlashControlTest, AutoFlash) {
    FlashControl control(true);
    const uint8_t mode = ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH;
    ASSERT_TRUE(control.WantsBrightness(Settings(mode)));

    // Bright: no flash, not even for the still capture.
    control.SetBrightness(120);
    auto plan = control.Begin(Settings(mode), 0);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_CONVERGED);
    plan = control.Begin(Still(mode), kFrameNs);
    EXPECT_FALSE(plan.lit);

    // Dark: the flash is required, and fires.
    control.SetBrightness(10);
    plan = control.Begin(Settings(mode), 2 * kFrameNs);
    EXPECT_FALSE(plan.lit);
    EXPECT_EQ(plan.ae_state, ANDROID_CONTROL_AE_STATE_FLASH_REQUIRED);
    plan = control.Begin(Still(mode), 3 * kFrameNs);
    EXPECT_TRUE(plan.lit);
    EXPECT_EQ(plan.not_before_ns, 3 * kFrameNs + FlashControl::kSettleNs);
    EXPECT_FALSE(control.WantsBrightness(Settings(mode)));
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
