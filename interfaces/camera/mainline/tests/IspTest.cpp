/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <cstdlib>

#include <gtest/gtest.h>

#include "isp/BayerFormat.h"
#include "isp/Isp3A.h"
#include "isp/SoftIsp.h"
#include "tests/FakeVideoDevice.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

TEST(BayerFormatTest, Formats) {
    auto grbg = GetBayerFormat(V4L2_PIX_FMT_SGRBG10P);
    ASSERT_TRUE(grbg.has_value());
    EXPECT_EQ(grbg->bits, 10);
    EXPECT_EQ(grbg->packing, BayerPacking::kMipi10);
    EXPECT_EQ(grbg->ColorAt(0, 0), BayerColor::kGreenRed);
    EXPECT_EQ(grbg->ColorAt(1, 0), BayerColor::kRed);
    EXPECT_EQ(grbg->ColorAt(0, 1), BayerColor::kBlue);
    EXPECT_EQ(grbg->ColorAt(3, 3), BayerColor::kGreenBlue);
    EXPECT_EQ(grbg->RowBytes(8), 10u);

    auto bggr = GetBayerFormat(V4L2_PIX_FMT_SBGGR8);
    ASSERT_TRUE(bggr.has_value());
    EXPECT_EQ(bggr->ColorAt(0, 0), BayerColor::kBlue);
    EXPECT_EQ(bggr->ColorAt(1, 1), BayerColor::kRed);

    EXPECT_FALSE(GetBayerFormat(V4L2_PIX_FMT_SGRBG10DPCM8).has_value());
    EXPECT_FALSE(GetBayerFormat(V4L2_PIX_FMT_YUYV).has_value());
}

TEST(BayerFormatTest, Unpack) {
    uint16_t out[4];

    // 0x3ff, 0x000, 0x155, 0x2aa
    const uint8_t mipi10[] = {0xff, 0x00, 0x55, 0xaa, 0b10'01'00'11};
    UnpackBayerRow(*GetBayerFormat(V4L2_PIX_FMT_SRGGB10P), mipi10, 4, out);
    EXPECT_EQ(out[0], 0x3ff);
    EXPECT_EQ(out[1], 0x000);
    EXPECT_EQ(out[2], 0x155);
    EXPECT_EQ(out[3], 0x2aa);

    // 0xabc, 0x123
    const uint8_t mipi12[] = {0xab, 0x12, 0x3c};
    UnpackBayerRow(*GetBayerFormat(V4L2_PIX_FMT_SRGGB12P), mipi12, 2, out);
    EXPECT_EQ(out[0], 0xabc);
    EXPECT_EQ(out[1], 0x123);

    // Little endian, upper bits ignored.
    const uint8_t le16[] = {0xff, 0xff, 0x01, 0x02};
    UnpackBayerRow(*GetBayerFormat(V4L2_PIX_FMT_SRGGB10), le16, 2, out);
    EXPECT_EQ(out[0], 0x3ff);
    EXPECT_EQ(out[1], 0x201);
}

// An 8 bit mosaic with one value per colour.
std::vector<uint8_t> Mosaic(const BayerFormat& format, int width, int height, uint8_t red,
                            uint8_t green, uint8_t blue) {
    std::vector<uint8_t> mosaic(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const BayerColor color = format.ColorAt(x, y);
            mosaic[static_cast<size_t>(y) * width + x] = color == BayerColor::kRed    ? red
                                                         : color == BayerColor::kBlue ? blue
                                                                                      : green;
        }
    }
    return mosaic;
}

TEST(DemosaicTest, FlatColor) {
    for (const uint32_t fourcc :
         {V4L2_PIX_FMT_SBGGR8, V4L2_PIX_FMT_SGBRG8, V4L2_PIX_FMT_SGRBG8, V4L2_PIX_FMT_SRGGB8}) {
        const BayerFormat format = *GetBayerFormat(fourcc);
        constexpr int kWidth = 8;
        constexpr int kHeight = 6;
        const auto mosaic = Mosaic(format, kWidth, kHeight, 200, 100, 50);
        std::vector<uint8_t> argb(kWidth * kHeight * 4);
        DemosaicBilinear(mosaic.data(), kWidth, kHeight, format, argb.data());
        // Every pixel, borders included.
        for (int i = 0; i < kWidth * kHeight; ++i) {
            EXPECT_EQ(argb[4 * i], 50) << "pixel " << i;
            EXPECT_EQ(argb[4 * i + 1], 100) << "pixel " << i;
            EXPECT_EQ(argb[4 * i + 2], 200) << "pixel " << i;
            EXPECT_EQ(argb[4 * i + 3], 255) << "pixel " << i;
        }
    }
}

class SoftIspTest : public ::testing::Test {
  protected:
    // An 8 bit GRBG frame with one value per colour.
    void Run(uint8_t red, uint8_t green, uint8_t blue, const IspParams& params = {}) {
        const BayerFormat bayer = *GetBayerFormat(V4L2_PIX_FMT_SGRBG8);
        data_ = Mosaic(bayer, kWidth, kHeight, red, green, blue);
        CaptureFormat format;
        format.fourcc = V4L2_PIX_FMT_SGRBG8;
        format.width = kWidth;
        format.height = kHeight;
        format.planes = {{kWidth, kWidth * kHeight}};
        CapturedFrame frame;
        frame.planes = {{data_.data(), data_.size()}};
        ASSERT_TRUE(isp_.Process(format, frame, params, &image_));
    }

    static constexpr int kWidth = 64;
    static constexpr int kHeight = 48;
    SoftIsp isp_;
    std::vector<uint8_t> data_;
    I420Image image_;
};

TEST_F(SoftIspTest, Gray) {
    Run(128, 128, 128);
    const IspStats& stats = isp_.stats();
    ASSERT_TRUE(stats.valid);
    // (128 - 16) / (255 - 16)
    EXPECT_NEAR(stats.red, 0.4686, 0.001);
    EXPECT_NEAR(stats.green, 0.4686, 0.001);
    EXPECT_NEAR(stats.blue, 0.4686, 0.001);
    EXPECT_EQ(stats.saturated, 0.0);

    ASSERT_EQ(image_.width(), kWidth);
    ASSERT_EQ(image_.height(), kHeight);
    // Gray: neutral chroma.
    EXPECT_NEAR(image_.u()[0], 128, 2);
    EXPECT_NEAR(image_.v()[0], 128, 2);
}

TEST_F(SoftIspTest, WhiteBalanceGains) {
    // Too red and not blue enough, until balanced.
    Run(200, 100, 50);
    const uint8_t v_unbalanced = image_.v()[0];
    EXPECT_GT(isp_.stats().red, isp_.stats().green);
    EXPECT_LT(isp_.stats().blue, isp_.stats().green);

    IspParams params;
    params.red_gain = isp_.stats().green / isp_.stats().red;
    params.blue_gain = isp_.stats().green / isp_.stats().blue;
    Run(200, 100, 50, params);
    EXPECT_LT(image_.v()[0], v_unbalanced);
    EXPECT_NEAR(image_.u()[0], 128, 3);
    EXPECT_NEAR(image_.v()[0], 128, 3);
    // The statistics do not depend on the gains.
    EXPECT_GT(isp_.stats().red, isp_.stats().green);
}

TEST_F(SoftIspTest, Clipped) {
    Run(255, 255, 255);
    EXPECT_EQ(isp_.stats().saturated, 1.0);
}

TEST_F(SoftIspTest, ShortFrame) {
    CaptureFormat format;
    format.fourcc = V4L2_PIX_FMT_SGRBG8;
    format.width = kWidth;
    format.height = kHeight;
    format.planes = {{kWidth, kWidth * kHeight}};
    std::vector<uint8_t> data(kWidth);
    CapturedFrame frame;
    frame.planes = {{data.data(), data.size()}};
    EXPECT_FALSE(isp_.Process(format, frame, {}, &image_));
}

IspStats Stats(double luminance, double saturated = 0.0) {
    return {.valid = true,
            .red = luminance,
            .green = luminance,
            .blue = luminance,
            .saturated = saturated};
}

class Isp3ATest : public ::testing::Test {
  protected:
    void SetUp() override {
        sensor_.controls()[V4L2_CID_EXPOSURE] = 100;
        sensor_.ranges()[V4L2_CID_EXPOSURE] = {4, 1000, 1, 100};
        sensor_.controls()[V4L2_CID_ANALOGUE_GAIN] = 128;
        sensor_.ranges()[V4L2_CID_ANALOGUE_GAIN] = {128, 1024, 1, 128};
        isp_3a_.Start();
    }

    // Feeds the same statistics until AE has applied a change and waited
    // for it.
    void Step(const IspStats& stats) {
        for (int i = 0; i <= Isp3A::kSettleFrames; ++i) isp_3a_.Update(stats);
    }

    int32_t exposure() { return *sensor_.GetControl(V4L2_CID_EXPOSURE); }
    int32_t gain() { return *sensor_.GetControl(V4L2_CID_ANALOGUE_GAIN); }

    FakeVideoDevice sensor_{FakeVideoDevice::UvcInfo("v4l-subdev0", "/sys/devices/test"), {}};
    Isp3A isp_3a_{&sensor_};
};

TEST_F(Isp3ATest, GrayWorld) {
    isp_3a_.SetLocks(/*ae=*/true, /*awb=*/false);
    isp_3a_.Update({.valid = true, .red = 0.1, .green = 0.2, .blue = 0.4});
    EXPECT_DOUBLE_EQ(isp_3a_.params().red_gain, 2.0);
    EXPECT_DOUBLE_EQ(isp_3a_.params().blue_gain, 0.5);
    // Then gradually.
    isp_3a_.Update({.valid = true, .red = 0.2, .green = 0.2, .blue = 0.2});
    EXPECT_GT(isp_3a_.params().red_gain, 1.0);
    EXPECT_LT(isp_3a_.params().red_gain, 2.0);
}

TEST_F(Isp3ATest, ConvergedDoesNothing) {
    isp_3a_.Update(Stats(Isp3A::kTargetLuminance * 1.05));
    EXPECT_EQ(exposure(), 100);
    EXPECT_EQ(gain(), 128);
    EXPECT_EQ(isp_3a_.params().digital_gain, 1.0);
}

TEST_F(Isp3ATest, DarkRaisesExposureThenGain) {
    Step(Stats(Isp3A::kTargetLuminance / 4));
    EXPECT_GT(exposure(), 100);
    EXPECT_EQ(gain(), 128);

    // Pitch dark: exposure, then gain, then digital gain run out.
    for (int i = 0; i < 20; ++i) Step(Stats(0.001));
    EXPECT_EQ(exposure(), 1000);
    EXPECT_EQ(gain(), 1024);
    EXPECT_DOUBLE_EQ(isp_3a_.params().digital_gain, Isp3A::kMaxDigitalGain);
}

TEST_F(Isp3ATest, ClippedLowersExposure) {
    Step(Stats(Isp3A::kTargetLuminance, /*saturated=*/0.3));
    EXPECT_LT(exposure(), 100);
}

TEST_F(Isp3ATest, WaitsForChangesToShow) {
    isp_3a_.Update(Stats(Isp3A::kTargetLuminance / 4));
    const int32_t changed = exposure();
    EXPECT_NE(changed, 100);
    // The next frames still show the old exposure.
    isp_3a_.Update(Stats(Isp3A::kTargetLuminance / 4));
    EXPECT_EQ(exposure(), changed);
}

TEST_F(Isp3ATest, Locked) {
    isp_3a_.SetLocks(true, true);
    isp_3a_.Update({.valid = true, .red = 0.01, .green = 0.02, .blue = 0.04});
    EXPECT_EQ(exposure(), 100);
    EXPECT_EQ(isp_3a_.params().red_gain, 1.0);
}

TEST(Isp3AWithoutSensorTest, DigitalGainOnly) {
    Isp3A isp_3a(nullptr);
    isp_3a.Start();
    isp_3a.Update(Stats(Isp3A::kTargetLuminance / 2));
    EXPECT_GT(isp_3a.params().digital_gain, 1.0);
    Isp3A bright(nullptr);
    bright.Start();
    bright.Update(Stats(Isp3A::kTargetLuminance * 2));
    EXPECT_LT(bright.params().digital_gain, 1.0);
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
