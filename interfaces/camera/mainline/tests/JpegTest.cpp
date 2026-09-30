/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cstring>

#include <aidl/android/hardware/camera/device/CameraBlob.h>
#include <aidl/android/hardware/camera/device/CameraBlobId.h>
#include <gtest/gtest.h>
#include <libyuv/convert.h>
#include <system/camera_metadata.h>

#include "jpeg/JpegEncoder.h"
#include "jpeg/JpegOutput.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

using device::CameraBlob;
using device::CameraBlobId;

I420Image Uniform(int width, int height, uint8_t y, uint8_t u, uint8_t v, bool full_range) {
    I420Image image;
    image.Resize(width, height);
    std::fill(image.y(), image.y() + width * height, y);
    const size_t chroma = static_cast<size_t>(image.uv_stride()) * ((height + 1) / 2);
    std::fill(image.u(), image.u() + chroma, u);
    std::fill(image.v(), image.v() + chroma, v);
    image.full_range = full_range;
    return image;
}

bool Contains(const uint8_t* data, size_t size, const std::vector<uint8_t>& needle) {
    return std::search(data, data + size, needle.begin(), needle.end()) != data + size;
}

TEST(JpegEncoderTest, RoundTrip) {
    // Odd sizes exercise the padding of partial MCUs.
    for (const Size size : {Size{64, 48}, Size{70, 38}}) {
        const I420Image image = Uniform(size.width, size.height, 120, 90, 160, true);
        std::vector<uint8_t> jpeg(64 * 1024);
        const size_t encoded = EncodeJpeg(image, 95, {}, jpeg.data(), jpeg.size());
        ASSERT_GT(encoded, 4u);
        EXPECT_EQ(jpeg[0], 0xff);
        EXPECT_EQ(jpeg[1], 0xd8);
        EXPECT_EQ(jpeg[encoded - 2], 0xff);
        EXPECT_EQ(jpeg[encoded - 1], 0xd9);

        I420Image decoded;
        decoded.Resize(size.width, size.height);
        ASSERT_EQ(libyuv::MJPGToI420(jpeg.data(), encoded, decoded.y(), decoded.y_stride(),
                                     decoded.u(), decoded.uv_stride(), decoded.v(),
                                     decoded.uv_stride(), size.width, size.height, size.width,
                                     size.height),
                  0);
        EXPECT_NEAR(decoded.y()[0], 120, 3);
        EXPECT_NEAR(decoded.u()[0], 90, 3);
        EXPECT_NEAR(decoded.v()[0], 160, 3);
    }
}

TEST(JpegEncoderTest, App1) {
    const I420Image image = Uniform(32, 32, 100, 128, 128, true);
    const std::vector<uint8_t> app1 = {'E', 'x', 'i', 'f', 0, 0, 'x', 'y', 'z'};
    std::vector<uint8_t> jpeg(32 * 1024);
    const size_t encoded = EncodeJpeg(image, 90, app1, jpeg.data(), jpeg.size());
    ASSERT_GT(encoded, 0u);
    // APP1 marker, length (payload plus the two length bytes), payload.
    EXPECT_TRUE(Contains(jpeg.data(), encoded, {0xff, 0xe1, 0x00, 11, 'E', 'x', 'i', 'f'}));

    // Too large for a JPEG segment.
    EXPECT_EQ(EncodeJpeg(image, 90, std::vector<uint8_t>(70000), jpeg.data(), jpeg.size()), 0u);
}

TEST(JpegEncoderTest, DoesNotOverflow) {
    const I420Image image = Uniform(256, 256, 100, 128, 128, true);
    std::vector<uint8_t> jpeg(64, 0);
    jpeg.resize(128, 0x5a);  // guard
    EXPECT_EQ(EncodeJpeg(image, 90, {}, jpeg.data(), 64), 0u);
    for (size_t i = 64; i < jpeg.size(); ++i) ASSERT_EQ(jpeg[i], 0x5a);
}

TEST(JpegEncoderTest, ToFullRange) {
    I420Image image = Uniform(4, 4, 16, 128, 240, false);
    ToFullRange(&image);
    EXPECT_TRUE(image.full_range);
    EXPECT_EQ(image.y()[0], 0);
    EXPECT_EQ(image.u()[0], 128);
    EXPECT_EQ(image.v()[0], 255);

    image = Uniform(4, 4, 235, 16, 128, false);
    ToFullRange(&image);
    EXPECT_EQ(image.y()[0], 255);
    EXPECT_NEAR(image.u()[0], 0, 1);

    // Already full range: untouched.
    image = Uniform(4, 4, 16, 16, 16, true);
    ToFullRange(&image);
    EXPECT_EQ(image.y()[0], 16);
}

TEST(WriteJpegTest, BlobWithTrailerAndExif) {
    const I420Image source = Uniform(640, 480, 120, 128, 128, false);
    Metadata settings;
    settings.SetU8(ANDROID_JPEG_QUALITY, 90);
    settings.SetU8(ANDROID_JPEG_THUMBNAIL_QUALITY, 80);
    settings.Set(ANDROID_JPEG_THUMBNAIL_SIZE, std::vector<int32_t>{160, 120});
    settings.SetI32(ANDROID_JPEG_ORIENTATION, 90);
    Metadata characteristics;
    characteristics.SetU8(ANDROID_FLASH_INFO_AVAILABLE, ANDROID_FLASH_INFO_AVAILABLE_FALSE);
    JpegContext context{&characteristics, "Make", "Model"};
    JpegWorkspace workspace;

    std::vector<uint8_t> buffer(256 * 1024, 0);
    ASSERT_TRUE(WriteJpeg(source, {0, 0, 640, 480}, {320, 240}, settings, context, buffer.data(),
                          buffer.size(), &workspace));

    CameraBlob blob;
    memcpy(&blob, buffer.data() + buffer.size() - sizeof(CameraBlob), sizeof(CameraBlob));
    EXPECT_EQ(blob.blobId, CameraBlobId::JPEG);
    ASSERT_GT(blob.blobSizeBytes, 4);
    ASSERT_LT(static_cast<size_t>(blob.blobSizeBytes), buffer.size() - sizeof(CameraBlob));
    EXPECT_EQ(buffer[0], 0xff);
    EXPECT_EQ(buffer[1], 0xd8);
    EXPECT_EQ(buffer[blob.blobSizeBytes - 1], 0xd9);
    // EXIF with the make, and an embedded thumbnail (a second SOI).
    EXPECT_TRUE(Contains(buffer.data(), blob.blobSizeBytes, {'E', 'x', 'i', 'f', 0, 0}));
    EXPECT_TRUE(Contains(buffer.data(), blob.blobSizeBytes, {'M', 'a', 'k', 'e', 0}));
    EXPECT_TRUE(Contains(buffer.data() + 2, blob.blobSizeBytes - 2, {0xff, 0xd8}));
}

TEST(WriteJpegTest, NoThumbnail) {
    const I420Image source = Uniform(64, 48, 120, 128, 128, true);
    Metadata settings;
    settings.Set(ANDROID_JPEG_THUMBNAIL_SIZE, std::vector<int32_t>{0, 0});
    JpegWorkspace workspace;
    std::vector<uint8_t> buffer(64 * 1024, 0);
    ASSERT_TRUE(WriteJpeg(source, {0, 0, 64, 48}, {64, 48}, settings, JpegContext{}, buffer.data(),
                          buffer.size(), &workspace));
    EXPECT_TRUE(workspace.thumbnail_jpeg.empty());
}

TEST(WriteJpegTest, BufferTooSmall) {
    const I420Image source = Uniform(640, 480, 120, 128, 128, true);
    JpegWorkspace workspace;
    std::vector<uint8_t> buffer(256, 0);
    EXPECT_FALSE(WriteJpeg(source, {0, 0, 640, 480}, {640, 480}, Metadata(), JpegContext{},
                           buffer.data(), buffer.size(), &workspace));
    EXPECT_FALSE(WriteJpeg(source, {0, 0, 640, 480}, {640, 480}, Metadata(), JpegContext{},
                           buffer.data(), 4, &workspace));
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
