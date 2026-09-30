/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <algorithm>

#include <gtest/gtest.h>

#include "convert/FormatConverter.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

constexpr int kWidth = 8;
constexpr int kHeight = 4;

CaptureFormat Format(uint32_t fourcc, uint32_t stride, uint32_t size) {
    return {fourcc, kWidth, kHeight, {{stride, size}}};
}

CapturedFrame Frame(const std::vector<uint8_t>& data) {
    CapturedFrame frame;
    frame.planes.push_back({data.data(), data.size()});
    return frame;
}

// Every luma sample of `image` is close to `y`, every chroma sample to `u`/`v`.
void ExpectUniform(const I420Image& image, int y, int u, int v, int tolerance = 2) {
    for (int row = 0; row < image.height(); ++row) {
        for (int col = 0; col < image.width(); ++col) {
            EXPECT_NEAR(image.y()[row * image.y_stride() + col], y, tolerance);
        }
    }
    for (int row = 0; row < (image.height() + 1) / 2; ++row) {
        for (int col = 0; col < (image.width() + 1) / 2; ++col) {
            EXPECT_NEAR(image.u()[row * image.uv_stride() + col], u, tolerance);
            EXPECT_NEAR(image.v()[row * image.uv_stride() + col], v, tolerance);
        }
    }
}

TEST(ConvertToI420Test, PackedYuv) {
    struct Case {
        uint32_t fourcc;
        uint8_t pattern[4];
    };
    // Y = 100, U = 50, V = 200 in every layout.
    const Case cases[] = {
            {V4L2_PIX_FMT_YUYV, {100, 50, 100, 200}},
            {V4L2_PIX_FMT_YVYU, {100, 200, 100, 50}},
            {V4L2_PIX_FMT_UYVY, {50, 100, 200, 100}},
            {V4L2_PIX_FMT_VYUY, {200, 100, 50, 100}},
    };
    for (const auto& c : cases) {
        std::vector<uint8_t> data;
        for (int i = 0; i < kWidth * kHeight / 2; ++i) {
            data.insert(data.end(), std::begin(c.pattern), std::end(c.pattern));
        }
        I420Image image;
        ASSERT_TRUE(ConvertToI420(Format(c.fourcc, kWidth * 2, data.size()), Frame(data), &image));
        EXPECT_EQ(image.size(), (Size{kWidth, kHeight}));
        EXPECT_FALSE(image.full_range);
        ExpectUniform(image, 100, 50, 200, 0);
    }
}

TEST(ConvertToI420Test, SemiPlanarAndPlanar) {
    const int luma = kWidth * kHeight;
    // NV12: Y plane, then U/V pairs.
    std::vector<uint8_t> nv12(luma, 100);
    for (int i = 0; i < luma / 4; ++i) nv12.insert(nv12.end(), {50, 200});
    // NV21: V/U pairs.
    std::vector<uint8_t> nv21(luma, 100);
    for (int i = 0; i < luma / 4; ++i) nv21.insert(nv21.end(), {200, 50});
    // YU12: Y, U, V planes.
    std::vector<uint8_t> yu12(luma, 100);
    yu12.insert(yu12.end(), luma / 4, 50);
    yu12.insert(yu12.end(), luma / 4, 200);
    // YV12: Y, V, U planes.
    std::vector<uint8_t> yv12(luma, 100);
    yv12.insert(yv12.end(), luma / 4, 200);
    yv12.insert(yv12.end(), luma / 4, 50);
    // NV16: full height U/V pairs.
    std::vector<uint8_t> nv16(luma, 100);
    for (int i = 0; i < luma / 2; ++i) nv16.insert(nv16.end(), {50, 200});

    const std::pair<uint32_t, const std::vector<uint8_t>*> cases[] = {
            {V4L2_PIX_FMT_NV12, &nv12},   {V4L2_PIX_FMT_NV21, &nv21}, {V4L2_PIX_FMT_YUV420, &yu12},
            {V4L2_PIX_FMT_YVU420, &yv12}, {V4L2_PIX_FMT_NV16, &nv16},
    };
    for (const auto& [fourcc, data] : cases) {
        I420Image image;
        ASSERT_TRUE(ConvertToI420(Format(fourcc, kWidth, data->size()), Frame(*data), &image))
                << fourcc;
        ExpectUniform(image, 100, 50, 200, 0);
    }
}

TEST(ConvertToI420Test, Grey) {
    std::vector<uint8_t> data(kWidth * kHeight, 77);
    I420Image image;
    ASSERT_TRUE(ConvertToI420(Format(V4L2_PIX_FMT_GREY, kWidth, data.size()), Frame(data), &image));
    ExpectUniform(image, 77, 128, 128, 0);
}

TEST(ConvertToI420Test, Rgb) {
    // Pure white and black in the 24 and 32 bit layouts.
    std::vector<uint8_t> white24(kWidth * kHeight * 3, 255);
    std::vector<uint8_t> black32(kWidth * kHeight * 4, 0);
    for (size_t i = 3; i < black32.size(); i += 4) black32[i] = 255;  // alpha

    I420Image image;
    ASSERT_TRUE(ConvertToI420(Format(V4L2_PIX_FMT_RGB24, kWidth * 3, white24.size()),
                              Frame(white24), &image));
    ExpectUniform(image, 235, 128, 128);
    ASSERT_TRUE(ConvertToI420(Format(V4L2_PIX_FMT_BGR24, kWidth * 3, white24.size()),
                              Frame(white24), &image));
    ExpectUniform(image, 235, 128, 128);
    ASSERT_TRUE(ConvertToI420(Format(V4L2_PIX_FMT_RGBA32, kWidth * 4, black32.size()),
                              Frame(black32), &image));
    ExpectUniform(image, 16, 128, 128);
}

TEST(ConvertToI420Test, RgbChannelOrder) {
    // Pure red: V (Cr) high, U (Cb) low, whatever the byte order.
    std::vector<uint8_t> rgb24, bgr24;
    for (int i = 0; i < kWidth * kHeight; ++i) {
        rgb24.insert(rgb24.end(), {255, 0, 0});
        bgr24.insert(bgr24.end(), {0, 0, 255});
    }
    for (const auto& [fourcc, data] :
         {std::make_pair(V4L2_PIX_FMT_RGB24, &rgb24), std::make_pair(V4L2_PIX_FMT_BGR24, &bgr24)}) {
        I420Image image;
        ASSERT_TRUE(ConvertToI420(Format(fourcc, kWidth * 3, data->size()), Frame(*data), &image));
        EXPECT_GT(image.v()[0], 200) << fourcc;
        EXPECT_LT(image.u()[0], 128) << fourcc;
    }
}

TEST(ConvertToI420Test, RejectsBadFrames) {
    std::vector<uint8_t> data(kWidth * kHeight * 2 - 1, 0);
    I420Image image;
    // Short.
    EXPECT_FALSE(
            ConvertToI420(Format(V4L2_PIX_FMT_YUYV, kWidth * 2, data.size()), Frame(data), &image));
    // Flagged by the driver.
    data.resize(kWidth * kHeight * 2);
    CapturedFrame flagged = Frame(data);
    flagged.error = true;
    EXPECT_FALSE(
            ConvertToI420(Format(V4L2_PIX_FMT_YUYV, kWidth * 2, data.size()), flagged, &image));
    // Garbage instead of a JPEG.
    EXPECT_FALSE(ConvertToI420(Format(V4L2_PIX_FMT_MJPEG, 0, data.size()), Frame(data), &image));
    // Unsupported format.
    EXPECT_FALSE(ConvertToI420(Format(V4L2_PIX_FMT_SGRBG10, kWidth * 2, data.size()), Frame(data),
                               &image));
    // No planes.
    EXPECT_FALSE(ConvertToI420(Format(V4L2_PIX_FMT_YUYV, kWidth * 2, data.size()), CapturedFrame{},
                               &image));
}

TEST(CenterCropToAspectTest, Crops) {
    const Rect full = {0, 0, 1920, 1080};
    EXPECT_EQ(CenterCropToAspect(full, {1920, 1080}), full);
    EXPECT_EQ(CenterCropToAspect(full, {1280, 720}), full);
    // 4:3 out of 16:9: cut the sides.
    EXPECT_EQ(CenterCropToAspect(full, {640, 480}), (Rect{240, 0, 1440, 1080}));
    // 16:9 out of 4:3: cut top and bottom.
    EXPECT_EQ(CenterCropToAspect({0, 0, 640, 480}, {1280, 720}), (Rect{0, 60, 640, 360}));
    // Offsets are kept and aligned.
    const Rect crop = CenterCropToAspect({101, 51, 400, 300}, {400, 400});
    EXPECT_EQ(crop.width, 300);
    EXPECT_EQ(crop.height, 300);
    EXPECT_EQ(crop.x % 2, 0);
    EXPECT_EQ(crop.y % 2, 0);
}

class ScaleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        source_.Resize(16, 8);
        std::fill(source_.y(), source_.y() + 16 * 8, 100);
        std::fill(source_.u(), source_.u() + 8 * 4, 50);
        std::fill(source_.v(), source_.v() + 8 * 4, 200);
    }

    I420Image source_;
    I420Image scratch_;
};

TEST_F(ScaleTest, ToPlanarAndSemiPlanar) {
    const Size out = {8, 4};
    std::vector<uint8_t> memory(8 * 4 * 2, 0);
    uint8_t* y = memory.data();
    uint8_t* c = memory.data() + 32;

    // Planar.
    YuvDestination planar = {y, c, c + 8, 8, 4, 1};
    ASSERT_TRUE(ScaleToYuv(source_, {0, 0, 16, 8}, out, planar, &scratch_));
    EXPECT_EQ(y[0], 100);
    EXPECT_EQ(c[0], 50);
    EXPECT_EQ(c[8], 200);

    // NV12 and NV21.
    YuvDestination nv12 = {y, c, c + 1, 8, 8, 2};
    ASSERT_TRUE(ScaleToYuv(source_, {0, 0, 16, 8}, out, nv12, &scratch_));
    EXPECT_EQ(c[0], 50);
    EXPECT_EQ(c[1], 200);
    YuvDestination nv21 = {y, c + 1, c, 8, 8, 2};
    ASSERT_TRUE(ScaleToYuv(source_, {0, 0, 16, 8}, out, nv21, &scratch_));
    EXPECT_EQ(c[0], 200);
    EXPECT_EQ(c[1], 50);

    // Interleaved but neither.
    YuvDestination odd = {y, c, c + 4, 8, 8, 2};
    EXPECT_FALSE(ScaleToYuv(source_, {0, 0, 16, 8}, out, odd, &scratch_));
}

TEST_F(ScaleTest, ToRgba) {
    std::vector<uint8_t> rgba(4 * 4 * 4, 0);
    ASSERT_TRUE(ScaleToRgba(source_, {0, 0, 8, 8}, {4, 4}, {rgba.data(), 16}, &scratch_));
    EXPECT_EQ(rgba[3], 255);  // opaque
    // Y 100, U 50, V 200 is a reddish color.
    EXPECT_GT(rgba[0], rgba[2]);
}

TEST_F(ScaleTest, Black) {
    FillBlack(&source_);
    EXPECT_TRUE(source_.full_range);
    I420Image black;
    black.Resize(4, 4);
    ScaleToI420(source_, {0, 0, 16, 8}, &black);
    EXPECT_EQ(black.y()[0], 0);
    EXPECT_EQ(black.u()[0], 128);
    EXPECT_EQ(black.v()[0], 128);

    std::vector<uint8_t> rgba(4 * 4 * 4, 0xee);
    ASSERT_TRUE(ScaleToRgba(source_, {0, 0, 8, 8}, {4, 4}, {rgba.data(), 16}, &scratch_));
    EXPECT_EQ(rgba[0], 0);
    EXPECT_EQ(rgba[1], 0);
    EXPECT_EQ(rgba[2], 0);
    EXPECT_EQ(rgba[3], 255);
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
