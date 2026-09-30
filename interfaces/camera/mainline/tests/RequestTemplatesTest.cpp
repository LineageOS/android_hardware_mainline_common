/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <algorithm>
#include <set>

#include <gtest/gtest.h>
#include <system/camera_metadata.h>

#include "device/RequestTemplates.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

using device::RequestTemplate;

std::shared_ptr<const CameraDescription> Describe(std::vector<FrameSize> sizes) {
    CameraCandidate candidate;
    candidate.key = "/sys/devices/test";
    FormatDescription format;
    format.fourcc = V4L2_PIX_FMT_YUYV;
    format.sizes = std::move(sizes);
    candidate.formats = {format};
    return CameraDescription::Create(candidate);
}

std::shared_ptr<const CameraDescription> Webcam() {
    return Describe({{1280, 720, {{1, 60}, {1, 30}, {1, 15}}}, {640, 480, {{1, 30}}}});
}

TEST(RequestTemplatesTest, OptionalTemplatesUnsupported) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    EXPECT_FALSE(BuildRequestTemplate(*description, RequestTemplate::ZERO_SHUTTER_LAG).has_value());
    EXPECT_FALSE(BuildRequestTemplate(*description, RequestTemplate::MANUAL).has_value());
}

TEST(RequestTemplatesTest, Intents) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    const std::pair<RequestTemplate, uint8_t> expected[] = {
            {RequestTemplate::PREVIEW, ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW},
            {RequestTemplate::STILL_CAPTURE, ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE},
            {RequestTemplate::VIDEO_RECORD, ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD},
            {RequestTemplate::VIDEO_SNAPSHOT, ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT},
    };
    for (const auto& [type, intent] : expected) {
        auto m = BuildRequestTemplate(*description, type);
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->GetU8(ANDROID_CONTROL_CAPTURE_INTENT), intent);
        EXPECT_EQ(m->GetFloat(ANDROID_CONTROL_ZOOM_RATIO), 1.0f);
        EXPECT_EQ(m->GetI32s(ANDROID_SCALER_CROP_REGION), (std::vector<int32_t>{0, 0, 1280, 720}));
    }
}

TEST(RequestTemplatesTest, OnlyAvailableRequestKeys) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    const auto listed =
            description->characteristics().GetI32s(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS);
    const std::set<int32_t> keys(listed.begin(), listed.end());
    auto m = BuildRequestTemplate(*description, RequestTemplate::PREVIEW);
    ASSERT_TRUE(m.has_value());
    for (const int32_t tag : m->Tags()) {
        EXPECT_EQ(keys.count(tag), 1u) << get_camera_metadata_tag_name(tag);
    }
}

TEST(RequestTemplatesTest, FpsRanges) {
    auto description = Webcam();
    ASSERT_NE(description, nullptr);
    const auto& available = description->fps_ranges();

    auto preview = BuildRequestTemplate(*description, RequestTemplate::PREVIEW);
    ASSERT_TRUE(preview.has_value());
    const auto preview_range = preview->GetI32s(ANDROID_CONTROL_AE_TARGET_FPS_RANGE);
    // Up to 30 fps, variable.
    EXPECT_EQ(preview_range, (std::vector<int32_t>{15, 30}));

    auto video = BuildRequestTemplate(*description, RequestTemplate::VIDEO_RECORD);
    ASSERT_TRUE(video.has_value());
    const auto video_range = video->GetI32s(ANDROID_CONTROL_AE_TARGET_FPS_RANGE);
    EXPECT_EQ(video_range, (std::vector<int32_t>{30, 30}));

    for (const auto& range : {preview_range, video_range}) {
        ASSERT_EQ(range.size(), 2u);
        EXPECT_NE(std::find(available.begin(), available.end(),
                            std::array<int32_t, 2>{range[0], range[1]}),
                  available.end());
    }
}

TEST(RequestTemplatesTest, OnlyFastFrameRates) {
    // All frame rates above 30 fps: the lowest one is used.
    auto description = Describe({{640, 480, {{1, 90}, {1, 60}}}});
    ASSERT_NE(description, nullptr);
    auto video = BuildRequestTemplate(*description, RequestTemplate::VIDEO_RECORD);
    ASSERT_TRUE(video.has_value());
    EXPECT_EQ(video->GetI32s(ANDROID_CONTROL_AE_TARGET_FPS_RANGE), (std::vector<int32_t>{60, 60}));
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
