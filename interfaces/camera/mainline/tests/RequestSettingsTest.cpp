/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <gtest/gtest.h>
#include <system/camera_metadata.h>

#include "session/RequestSettings.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

std::shared_ptr<const CameraDescription> Webcam() {
    CameraCandidate candidate;
    candidate.key = "/sys/devices/test";
    FormatDescription format;
    format.fourcc = V4L2_PIX_FMT_YUYV;
    format.sizes = {{1920, 1080, {{1, 30}, {1, 15}}}, {640, 480, {{1, 60}, {1, 30}}}};
    candidate.formats = {format};
    return CameraDescription::Create(candidate);
}

TEST(ParseSettingsTest, Defaults) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    const RequestSettings parsed = ParseSettings(Metadata(), *description);
    EXPECT_EQ(parsed.region, (Rect{0, 0, 1920, 1080}));
    EXPECT_EQ(parsed.result_zoom_ratio, 1.0f);
    EXPECT_FALSE(parsed.ae_lock);
    EXPECT_FALSE(parsed.awb_lock);
    EXPECT_FALSE(parsed.black);
    EXPECT_EQ(parsed.fps_range, (std::array<int32_t, 2>{15, 30}));
}

TEST(ParseSettingsTest, FpsRange) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    Metadata settings;
    settings.Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, std::vector<int32_t>{60, 60});
    EXPECT_EQ(ParseSettings(settings, *description).fps_range, (std::array<int32_t, 2>{60, 60}));
    // Not an available range: the default one.
    settings.Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, std::vector<int32_t>{24, 24});
    EXPECT_EQ(ParseSettings(settings, *description).fps_range, (std::array<int32_t, 2>{15, 30}));
}

TEST(ParseSettingsTest, ZoomRatio) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    Metadata settings;
    settings.SetFloat(ANDROID_CONTROL_ZOOM_RATIO, 2.0f);
    settings.Set(ANDROID_SCALER_CROP_REGION, std::vector<int32_t>{100, 100, 200, 200});
    RequestSettings parsed = ParseSettings(settings, *description);
    EXPECT_EQ(parsed.region, (Rect{480, 270, 960, 540}));
    EXPECT_EQ(parsed.result_zoom_ratio, 2.0f);
    // The crop region is reported as the full array when zooming by ratio.
    EXPECT_EQ(parsed.result_crop_region, (std::array<int32_t, 4>{0, 0, 1920, 1080}));

    // Clamped to the maximum.
    settings.SetFloat(ANDROID_CONTROL_ZOOM_RATIO, 100.0f);
    parsed = ParseSettings(settings, *description);
    EXPECT_EQ(parsed.result_zoom_ratio, description->max_zoom());
    EXPECT_EQ(parsed.region.width, 480);
}

TEST(ParseSettingsTest, CropRegion) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    Metadata settings;
    settings.Set(ANDROID_SCALER_CROP_REGION, std::vector<int32_t>{960, 540, 960, 540});
    RequestSettings parsed = ParseSettings(settings, *description);
    EXPECT_EQ(parsed.region, (Rect{960, 540, 960, 540}));
    EXPECT_EQ(parsed.result_crop_region, (std::array<int32_t, 4>{960, 540, 960, 540}));

    // Too small (more than the maximum zoom) and outside the array.
    settings.Set(ANDROID_SCALER_CROP_REGION, std::vector<int32_t>{1900, 1070, 10, 10});
    parsed = ParseSettings(settings, *description);
    EXPECT_EQ(parsed.region, (Rect{1440, 810, 480, 270}));
}

TEST(ParseSettingsTest, LocksAndTestPattern) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    Metadata settings;
    settings.SetU8(ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AE_LOCK_ON);
    settings.SetU8(ANDROID_CONTROL_AWB_LOCK, ANDROID_CONTROL_AWB_LOCK_ON);
    settings.SetI32(ANDROID_SENSOR_TEST_PATTERN_MODE, ANDROID_SENSOR_TEST_PATTERN_MODE_BLACK);
    RequestSettings parsed = ParseSettings(settings, *description);
    EXPECT_TRUE(parsed.ae_lock);
    EXPECT_TRUE(parsed.awb_lock);
    EXPECT_TRUE(parsed.black);

    settings.SetI32(ANDROID_SENSOR_TEST_PATTERN_MODE, ANDROID_SENSOR_TEST_PATTERN_MODE_SOLID_COLOR);
    EXPECT_TRUE(ParseSettings(settings, *description).black);
    settings.SetI32(ANDROID_SENSOR_TEST_PATTERN_MODE, ANDROID_SENSOR_TEST_PATTERN_MODE_OFF);
    EXPECT_FALSE(ParseSettings(settings, *description).black);
}

TEST(ToCaptureCoordinatesTest, Scales) {
    EXPECT_EQ(ToCaptureCoordinates({0, 0, 1920, 1080}, {1920, 1080}, {640, 480}),
              (Rect{0, 0, 640, 480}));
    EXPECT_EQ(ToCaptureCoordinates({480, 270, 960, 540}, {1920, 1080}, {1280, 720}),
              (Rect{320, 180, 640, 360}));
    // Degenerate input stays inside the frame.
    const Rect clamped = ToCaptureCoordinates({1919, 1079, 1, 1}, {1920, 1080}, {640, 480});
    EXPECT_GE(clamped.width, 2);
    EXPECT_LE(clamped.x + clamped.width, 640);
    EXPECT_LE(clamped.y + clamped.height, 480);
}

TEST(ChooseFrameIntervalTest, Picks) {
    const std::vector<Fraction> intervals = {{1, 60}, {1, 30}, {1, 15}};
    auto rate = [&](int32_t max) { return ChooseFrameInterval(intervals, max).denominator; };
    EXPECT_EQ(rate(60), 60u);
    EXPECT_EQ(rate(30), 30u);
    EXPECT_EQ(rate(29), 15u);
    EXPECT_EQ(rate(5), 15u);
    EXPECT_EQ(ChooseFrameInterval({{1001, 30000}}, 30).denominator, 30000u);
    EXPECT_EQ(ChooseFrameInterval({}, 30).denominator, 30u);
}

TEST(BuildResultTest, DynamicKeys) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    Metadata settings;
    settings.SetU8(ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AE_LOCK_ON);
    settings.SetU8(ANDROID_CONTROL_CAPTURE_INTENT, ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW);
    const RequestSettings parsed = ParseSettings(settings, *description);

    const Metadata result = BuildResult(settings, parsed, 12345, 2);
    EXPECT_EQ(result.GetI64(ANDROID_SENSOR_TIMESTAMP), 12345);
    EXPECT_EQ(result.GetU8(ANDROID_REQUEST_PIPELINE_DEPTH), 2);
    EXPECT_EQ(result.GetU8(ANDROID_CONTROL_AE_STATE), ANDROID_CONTROL_AE_STATE_LOCKED);
    EXPECT_EQ(result.GetU8(ANDROID_CONTROL_AWB_STATE), ANDROID_CONTROL_AWB_STATE_CONVERGED);
    EXPECT_EQ(result.GetU8(ANDROID_CONTROL_AF_STATE), ANDROID_CONTROL_AF_STATE_INACTIVE);
    // Settings are echoed.
    EXPECT_EQ(result.GetU8(ANDROID_CONTROL_CAPTURE_INTENT), ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW);
    EXPECT_EQ(result.GetI32s(ANDROID_SCALER_CROP_REGION), (std::vector<int32_t>{0, 0, 1920, 1080}));
    EXPECT_EQ(result.GetI32s(ANDROID_CONTROL_AE_TARGET_FPS_RANGE), (std::vector<int32_t>{15, 30}));
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
