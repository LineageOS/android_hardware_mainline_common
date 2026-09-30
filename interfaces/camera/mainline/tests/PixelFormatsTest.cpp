/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <gtest/gtest.h>

#include "v4l2/PixelFormats.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

TEST(PixelFormatsTest, Classify) {
    EXPECT_EQ(ClassifyPixelFormat(V4L2_PIX_FMT_YUYV), PixelFormatClass::kProcessed);
    EXPECT_EQ(ClassifyPixelFormat(V4L2_PIX_FMT_MJPEG), PixelFormatClass::kProcessed);
    EXPECT_EQ(ClassifyPixelFormat(V4L2_PIX_FMT_NV12), PixelFormatClass::kProcessed);
    EXPECT_EQ(ClassifyPixelFormat(V4L2_PIX_FMT_SGRBG10), PixelFormatClass::kBayer);
    EXPECT_EQ(ClassifyPixelFormat(V4L2_PIX_FMT_SRGGB10P), PixelFormatClass::kBayer);
    EXPECT_EQ(ClassifyPixelFormat(V4L2_PIX_FMT_IPU3_SBGGR10), PixelFormatClass::kBayer);
    EXPECT_EQ(ClassifyPixelFormat(V4L2_PIX_FMT_Z16), PixelFormatClass::kUnsupported);
    EXPECT_EQ(ClassifyPixelFormat(0), PixelFormatClass::kUnsupported);
}

TEST(PixelFormatsTest, Compressed) {
    EXPECT_TRUE(IsCompressedPixelFormat(V4L2_PIX_FMT_MJPEG));
    EXPECT_TRUE(IsCompressedPixelFormat(V4L2_PIX_FMT_JPEG));
    EXPECT_FALSE(IsCompressedPixelFormat(V4L2_PIX_FMT_YUYV));
}

TEST(PixelFormatsTest, FourccToString) {
    EXPECT_EQ(FourccToString(V4L2_PIX_FMT_YUYV), "YUYV");
    EXPECT_EQ(FourccToString(V4L2_PIX_FMT_MJPEG), "MJPG");
    EXPECT_EQ(FourccToString(v4l2_fourcc_be('Y', '1', '6', ' ')), "Y16 -BE");
    EXPECT_EQ(FourccToString(0), "????");
}

TEST(FractionTest, ToNanoseconds) {
    EXPECT_EQ((Fraction{1, 30}.ToNanoseconds()), 33'333'333);
    EXPECT_EQ((Fraction{1001, 30000}.ToNanoseconds()), 33'366'666);
    EXPECT_EQ((Fraction{0, 30}.ToNanoseconds()), 0);
    EXPECT_EQ((Fraction{1, 0}.ToNanoseconds()), 0);
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
