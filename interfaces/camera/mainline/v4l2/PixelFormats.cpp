/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "v4l2/PixelFormats.h"

#include <linux/videodev2.h>

namespace aidl::android::hardware::camera::mainline {

PixelFormatClass ClassifyPixelFormat(uint32_t fourcc) {
    switch (fourcc) {
        // Keep in sync with FormatConverter, which has to handle every one
        // of these.
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_YVYU:
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_VYUY:
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
        case V4L2_PIX_FMT_NV16:
        case V4L2_PIX_FMT_NV61:
        case V4L2_PIX_FMT_YUV420:
        case V4L2_PIX_FMT_YVU420:
        case V4L2_PIX_FMT_YUV422P:
        case V4L2_PIX_FMT_RGB24:
        case V4L2_PIX_FMT_BGR24:
        case V4L2_PIX_FMT_RGB565:
        case V4L2_PIX_FMT_ABGR32:
        case V4L2_PIX_FMT_XBGR32:
        case V4L2_PIX_FMT_ARGB32:
        case V4L2_PIX_FMT_XRGB32:
        case V4L2_PIX_FMT_RGBA32:
        case V4L2_PIX_FMT_RGBX32:
        case V4L2_PIX_FMT_GREY:
        case V4L2_PIX_FMT_MJPEG:
        case V4L2_PIX_FMT_JPEG:
            return PixelFormatClass::kProcessed;

        case V4L2_PIX_FMT_SBGGR8:
        case V4L2_PIX_FMT_SGBRG8:
        case V4L2_PIX_FMT_SGRBG8:
        case V4L2_PIX_FMT_SRGGB8:
        case V4L2_PIX_FMT_SBGGR10:
        case V4L2_PIX_FMT_SGBRG10:
        case V4L2_PIX_FMT_SGRBG10:
        case V4L2_PIX_FMT_SRGGB10:
        case V4L2_PIX_FMT_SBGGR10P:
        case V4L2_PIX_FMT_SGBRG10P:
        case V4L2_PIX_FMT_SGRBG10P:
        case V4L2_PIX_FMT_SRGGB10P:
        case V4L2_PIX_FMT_SBGGR10ALAW8:
        case V4L2_PIX_FMT_SGBRG10ALAW8:
        case V4L2_PIX_FMT_SGRBG10ALAW8:
        case V4L2_PIX_FMT_SRGGB10ALAW8:
        case V4L2_PIX_FMT_SBGGR10DPCM8:
        case V4L2_PIX_FMT_SGBRG10DPCM8:
        case V4L2_PIX_FMT_SGRBG10DPCM8:
        case V4L2_PIX_FMT_SRGGB10DPCM8:
        case V4L2_PIX_FMT_SBGGR12:
        case V4L2_PIX_FMT_SGBRG12:
        case V4L2_PIX_FMT_SGRBG12:
        case V4L2_PIX_FMT_SRGGB12:
        case V4L2_PIX_FMT_SBGGR12P:
        case V4L2_PIX_FMT_SGBRG12P:
        case V4L2_PIX_FMT_SGRBG12P:
        case V4L2_PIX_FMT_SRGGB12P:
        case V4L2_PIX_FMT_SBGGR14:
        case V4L2_PIX_FMT_SGBRG14:
        case V4L2_PIX_FMT_SGRBG14:
        case V4L2_PIX_FMT_SRGGB14:
        case V4L2_PIX_FMT_SBGGR14P:
        case V4L2_PIX_FMT_SGBRG14P:
        case V4L2_PIX_FMT_SGRBG14P:
        case V4L2_PIX_FMT_SRGGB14P:
        case V4L2_PIX_FMT_SBGGR16:
        case V4L2_PIX_FMT_SGBRG16:
        case V4L2_PIX_FMT_SGRBG16:
        case V4L2_PIX_FMT_SRGGB16:
        case V4L2_PIX_FMT_IPU3_SBGGR10:
        case V4L2_PIX_FMT_IPU3_SGBRG10:
        case V4L2_PIX_FMT_IPU3_SGRBG10:
        case V4L2_PIX_FMT_IPU3_SRGGB10:
            return PixelFormatClass::kBayer;

        default:
            return PixelFormatClass::kUnsupported;
    }
}

bool IsCompressedPixelFormat(uint32_t fourcc) {
    return fourcc == V4L2_PIX_FMT_MJPEG || fourcc == V4L2_PIX_FMT_JPEG;
}

std::string FourccToString(uint32_t fourcc) {
    // Big endian variants of a format carry an extra flag bit.
    constexpr uint32_t kBigEndianFlag = 1u << 31;
    const uint32_t code = fourcc & ~kBigEndianFlag;
    std::string result;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((code >> (8 * i)) & 0xff);
        result.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    if (fourcc & kBigEndianFlag) result += "-BE";
    return result;
}

}  // namespace aidl::android::hardware::camera::mainline
