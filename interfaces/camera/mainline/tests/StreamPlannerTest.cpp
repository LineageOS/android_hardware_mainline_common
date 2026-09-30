/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <gtest/gtest.h>

#include "device/StreamPlanner.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

FrameSize Frame(uint32_t width, uint32_t height, std::vector<Fraction> intervals) {
    return {width, height, std::move(intervals)};
}

FormatDescription Format(uint32_t fourcc, std::vector<FrameSize> sizes, bool emulated = false) {
    FormatDescription format;
    format.fourcc = fourcc;
    format.sizes = std::move(sizes);
    format.emulated = emulated;
    return format;
}

// A typical USB 2.0 webcam: YUYV is slow at high resolutions, MJPEG is not.
std::vector<FormatDescription> Webcam() {
    return {
            Format(V4L2_PIX_FMT_YUYV, {Frame(1920, 1080, {{1, 5}}), Frame(1280, 720, {{1, 10}}),
                                       Frame(640, 480, {{1, 30}, {1, 15}})}),
            Format(V4L2_PIX_FMT_MJPEG, {Frame(1920, 1080, {{1, 30}}), Frame(1280, 720, {{1, 30}}),
                                        Frame(640, 480, {{1, 30}})}),
    };
}

TEST(StreamPlannerTest, OutputSizesIncludeStandardSizes) {
    StreamPlanner planner(Webcam());
    EXPECT_EQ(planner.MaxSize(), (Size{1920, 1080}));
    EXPECT_TRUE(planner.IsOutputSize({1920, 1080}));
    EXPECT_TRUE(planner.IsOutputSize({640, 480}));
    // Standard sizes that fit.
    EXPECT_TRUE(planner.IsOutputSize({320, 240}));
    EXPECT_TRUE(planner.IsOutputSize({176, 144}));
    EXPECT_FALSE(planner.IsOutputSize({2560, 1440}));
    EXPECT_FALSE(planner.IsOutputSize({100, 100}));

    // Largest first, no duplicates.
    const auto& sizes = planner.OutputSizes();
    ASSERT_FALSE(sizes.empty());
    EXPECT_EQ(sizes.front().size, (Size{1920, 1080}));
    for (size_t i = 1; i < sizes.size(); ++i) {
        EXPECT_GE(sizes[i - 1].size.Area(), sizes[i].size.Area());
        EXPECT_NE(sizes[i - 1].size, sizes[i].size);
    }
}

TEST(StreamPlannerTest, OutputDurationIsFastestContainingMode) {
    StreamPlanner planner(Webcam());
    for (const auto& output : planner.OutputSizes()) {
        // MJPEG gives 30 fps everywhere.
        EXPECT_EQ(output.min_frame_duration_ns, 33'333'333) << output.size.width;
    }
}

TEST(StreamPlannerTest, PlanPrefersFrameRateThenSize) {
    StreamPlanner planner(Webcam());

    // 640x480: YUYV at 30 fps, no need to decode MJPEG.
    auto mode = planner.Plan({{640, 480}});
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(mode->fourcc, V4L2_PIX_FMT_YUYV);
    EXPECT_EQ(mode->size, (Size{640, 480}));

    // 1280x720: YUYV only does 10 fps, MJPEG 30.
    mode = planner.Plan({{1280, 720}, {320, 240}});
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(mode->fourcc, V4L2_PIX_FMT_MJPEG);
    EXPECT_EQ(mode->size, (Size{1280, 720}));

    // A 4:3 and a 16:9 output need a mode containing both.
    mode = planner.Plan({{1280, 720}, {640, 480}});
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(mode->size, (Size{1280, 720}));
}

TEST(StreamPlannerTest, PlanFailsWhenNothingFits) {
    StreamPlanner planner(Webcam());
    EXPECT_FALSE(planner.Plan({{2560, 1440}}).has_value());
    EXPECT_FALSE(planner.Plan({{1920, 1080}, {640, 1200}}).has_value());
}

TEST(StreamPlannerTest, PlanAvoidsEmulatedFormats) {
    StreamPlanner planner({
            Format(V4L2_PIX_FMT_RGB24, {Frame(640, 480, {{1, 30}})}, /*emulated=*/true),
            Format(V4L2_PIX_FMT_NV12, {Frame(640, 480, {{1, 30}})}),
    });
    auto mode = planner.Plan({{640, 480}});
    ASSERT_TRUE(mode.has_value());
    EXPECT_EQ(mode->fourcc, V4L2_PIX_FMT_NV12);
}

TEST(StreamPlannerTest, IgnoresUnusableFormats) {
    StreamPlanner planner({
            Format(V4L2_PIX_FMT_SGRBG10, {Frame(2592, 1944, {{1, 30}})}),
            Format(V4L2_PIX_FMT_YUYV,
                   {Frame(640, 480, {}), Frame(0, 0, {{1, 30}}), Frame(320, 240, {{1, 30}})}),
    });
    EXPECT_EQ(planner.MaxSize(), (Size{320, 240}));
    EXPECT_EQ(planner.Modes().size(), 1u);
}

TEST(StreamPlannerTest, FrameRates) {
    StreamPlanner planner(Webcam());
    EXPECT_EQ(planner.FrameRates(), (std::vector<int32_t>{5, 10, 15, 30}));

    StreamPlanner ntsc({Format(V4L2_PIX_FMT_YUYV, {
                                                          Frame(720, 480, {{1001, 30000}}),
                                                  })});
    EXPECT_EQ(ntsc.FrameRates(), (std::vector<int32_t>{30}));
}

TEST(StreamPlannerTest, Empty) {
    StreamPlanner planner({});
    EXPECT_TRUE(planner.OutputSizes().empty());
    EXPECT_FALSE(planner.Plan({{640, 480}}).has_value());
    EXPECT_TRUE(planner.FrameRates().empty());
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
