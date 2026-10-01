/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Convert"

#include "convert/FormatConverter.h"

#include <linux/videodev2.h>

#include <algorithm>

#include <android-base/logging.h>
#include <libyuv/convert.h>
#include <libyuv/convert_argb.h>
#include <libyuv/planar_functions.h>
#include <libyuv/scale.h>

#include "v4l2/PixelFormats.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

// Bytes per pixel of the first plane of packed formats, 0 for others.
int PackedBytesPerPixel(uint32_t fourcc) {
    switch (fourcc) {
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_YVYU:
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_VYUY:
        case V4L2_PIX_FMT_RGB565:
            return 2;
        case V4L2_PIX_FMT_RGB24:
        case V4L2_PIX_FMT_BGR24:
            return 3;
        case V4L2_PIX_FMT_ABGR32:
        case V4L2_PIX_FMT_XBGR32:
        case V4L2_PIX_FMT_ARGB32:
        case V4L2_PIX_FMT_XRGB32:
        case V4L2_PIX_FMT_RGBA32:
        case V4L2_PIX_FMT_RGBX32:
            return 4;
        default:
            return 0;
    }
}

// Bytes of a single plane frame of `fourcc` with luma stride `stride`.
size_t FrameBytes(uint32_t fourcc, size_t stride, size_t height) {
    const size_t luma = stride * height;
    switch (fourcc) {
        case V4L2_PIX_FMT_NV12:
        case V4L2_PIX_FMT_NV21:
            return luma + stride * ((height + 1) / 2);
        case V4L2_PIX_FMT_YUV420:
        case V4L2_PIX_FMT_YVU420:
            return luma + 2 * ((stride + 1) / 2) * ((height + 1) / 2);
        case V4L2_PIX_FMT_NV16:
        case V4L2_PIX_FMT_NV61:
        case V4L2_PIX_FMT_YUV422P:
            return 2 * luma;
        default:
            return luma;
    }
}

}  // namespace

bool ConvertToI420(const CaptureFormat& format, const CapturedFrame& frame, I420Image* out) {
    if (frame.error) {
        LOG(DEBUG) << "dropping frame " << frame.sequence << " flagged as erroneous";
        return false;
    }
    if (frame.planes.size() != 1 || format.planes.size() != 1) {
        LOG(ERROR) << "unexpected number of planes: " << frame.planes.size();
        return false;
    }

    const int w = static_cast<int>(format.width);
    const int h = static_cast<int>(format.height);
    const uint8_t* src = frame.planes[0].data;
    const size_t used = frame.planes[0].bytes_used;
    out->Resize(w, h);
    out->full_range = false;

    uint8_t* y = out->y();
    uint8_t* u = out->u();
    uint8_t* v = out->v();
    const int ys = out->y_stride();
    const int uvs = out->uv_stride();

    if (IsCompressedPixelFormat(format.fourcc)) {
        out->full_range = true;
        if (libyuv::MJPGToI420(src, used, y, ys, u, uvs, v, uvs, w, h, w, h) != 0) {
            LOG(DEBUG) << "dropping undecodable MJPEG frame " << frame.sequence << " (" << used
                       << " bytes)";
            return false;
        }
        return true;
    }

    int stride = static_cast<int>(format.planes[0].bytes_per_line);
    if (stride == 0) {
        const int bpp = PackedBytesPerPixel(format.fourcc);
        stride = bpp != 0 ? w * bpp : w;
    }
    if (used < FrameBytes(format.fourcc, stride, h)) {
        LOG(DEBUG) << "dropping short frame " << frame.sequence << ": " << used << " bytes";
        return false;
    }

    const uint8_t* chroma = src + static_cast<size_t>(stride) * h;
    const int chroma_stride = (stride + 1) / 2;
    int result;
    switch (format.fourcc) {
        case V4L2_PIX_FMT_YUYV:
            result = libyuv::YUY2ToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_YVYU:
            result = libyuv::YUY2ToI420(src, stride, y, ys, v, uvs, u, uvs, w, h);
            break;
        case V4L2_PIX_FMT_UYVY:
            result = libyuv::UYVYToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_VYUY:
            result = libyuv::UYVYToI420(src, stride, y, ys, v, uvs, u, uvs, w, h);
            break;
        case V4L2_PIX_FMT_NV12:
            result = libyuv::NV12ToI420(src, stride, chroma, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_NV21:
            result = libyuv::NV21ToI420(src, stride, chroma, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_NV16:
        case V4L2_PIX_FMT_NV61: {
            // 4:2:2 semi-planar: take every other chroma row.
            libyuv::CopyPlane(src, stride, y, ys, w, h);
            const bool swap = format.fourcc == V4L2_PIX_FMT_NV61;
            libyuv::SplitUVPlane(chroma, 2 * stride, swap ? v : u, uvs, swap ? u : v, uvs,
                                 (w + 1) / 2, (h + 1) / 2);
            result = 0;
            break;
        }
        case V4L2_PIX_FMT_YUV420:
        case V4L2_PIX_FMT_YVU420: {
            const uint8_t* first = chroma;
            const uint8_t* second = first + static_cast<size_t>(chroma_stride) * ((h + 1) / 2);
            const bool swap = format.fourcc == V4L2_PIX_FMT_YVU420;
            result = libyuv::I420Copy(src, stride, swap ? second : first, chroma_stride,
                                      swap ? first : second, chroma_stride, y, ys, u, uvs, v, uvs,
                                      w, h);
            break;
        }
        case V4L2_PIX_FMT_YUV422P: {
            const uint8_t* cb = chroma;
            const uint8_t* cr = cb + static_cast<size_t>(chroma_stride) * h;
            result = libyuv::I422ToI420(src, stride, cb, chroma_stride, cr, chroma_stride, y, ys, u,
                                        uvs, v, uvs, w, h);
            break;
        }
        case V4L2_PIX_FMT_GREY:
            result = libyuv::I400ToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        // libyuv names formats by their order in a little endian word,
        // V4L2 by their order in memory (except for the 32 bit ones).
        case V4L2_PIX_FMT_RGB24:  // R, G, B
            result = libyuv::RAWToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_BGR24:  // B, G, R
            result = libyuv::RGB24ToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_RGB565:
            result = libyuv::RGB565ToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_ABGR32:  // B, G, R, A
        case V4L2_PIX_FMT_XBGR32:
            result = libyuv::ARGBToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_ARGB32:  // A, R, G, B
        case V4L2_PIX_FMT_XRGB32:
            result = libyuv::BGRAToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        case V4L2_PIX_FMT_RGBA32:  // R, G, B, A
        case V4L2_PIX_FMT_RGBX32:
            result = libyuv::ABGRToI420(src, stride, y, ys, u, uvs, v, uvs, w, h);
            break;
        default:
            LOG(ERROR) << "no conversion for " << FourccToString(format.fourcc);
            return false;
    }
    if (result != 0) {
        LOG(ERROR) << "conversion of " << FourccToString(format.fourcc) << " failed";
        return false;
    }
    return true;
}

Rect CenterCropToAspect(const Rect& region, Size output) {
    Rect crop = region;
    if (output.width > 0 && output.height > 0) {
        const int64_t lhs = static_cast<int64_t>(region.width) * output.height;
        const int64_t rhs = static_cast<int64_t>(region.height) * output.width;
        if (lhs > rhs) {
            // Region is wider than the output: cut the sides.
            crop.width = static_cast<int32_t>(rhs / output.height);
        } else if (lhs < rhs) {
            crop.height = static_cast<int32_t>(lhs / output.width);
        }
    }
    crop.width = std::max(2, crop.width & ~1);
    crop.height = std::max(2, crop.height & ~1);
    crop.x = (region.x + (region.width - crop.width) / 2) & ~1;
    crop.y = (region.y + (region.height - crop.height) / 2) & ~1;
    return crop;
}

namespace {

struct Planes {
    const uint8_t* y;
    const uint8_t* u;
    const uint8_t* v;
};

Planes CropPlanes(const I420Image& image, const Rect& crop) {
    return {image.y() + static_cast<size_t>(crop.y) * image.y_stride() + crop.x,
            image.u() + static_cast<size_t>(crop.y / 2) * image.uv_stride() + crop.x / 2,
            image.v() + static_cast<size_t>(crop.y / 2) * image.uv_stride() + crop.x / 2};
}

// Crops and scales `source` into `scratch` resized to `output`, or returns the
// crop of `source` itself when no scaling is needed.
Planes CropScale(const I420Image& source, const Rect& crop, Size output, I420Image* scratch,
                 int* y_stride, int* uv_stride) {
    if (crop.width == output.width && crop.height == output.height) {
        *y_stride = source.y_stride();
        *uv_stride = source.uv_stride();
        return CropPlanes(source, crop);
    }
    scratch->Resize(output.width, output.height);
    scratch->full_range = source.full_range;
    ScaleToI420(source, crop, scratch);
    *y_stride = scratch->y_stride();
    *uv_stride = scratch->uv_stride();
    return {scratch->y(), scratch->u(), scratch->v()};
}

}  // namespace

void ScaleToI420(const I420Image& source, const Rect& crop, I420Image* destination) {
    const Planes src = CropPlanes(source, crop);
    libyuv::I420Scale(src.y, source.y_stride(), src.u, source.uv_stride(), src.v,
                      source.uv_stride(), crop.width, crop.height, destination->y(),
                      destination->y_stride(), destination->u(), destination->uv_stride(),
                      destination->v(), destination->uv_stride(), destination->width(),
                      destination->height(), libyuv::kFilterBilinear);
}

bool ScaleToYuv(const I420Image& source, const Rect& crop, Size output,
                const YuvDestination& destination, I420Image* scratch) {
    const int cw = (output.width + 1) / 2;
    const int ch = (output.height + 1) / 2;

    if (destination.c_step == 1) {
        // Planar: scale straight into the buffer.
        const Planes src = CropPlanes(source, crop);
        libyuv::I420Scale(src.y, source.y_stride(), src.u, source.uv_stride(), src.v,
                          source.uv_stride(), crop.width, crop.height, destination.y,
                          destination.y_stride, destination.cb, destination.c_stride,
                          destination.cr, destination.c_stride, output.width, output.height,
                          libyuv::kFilterBilinear);
        return true;
    }
    if (destination.c_step != 2) {
        LOG(ERROR) << "unsupported chroma step " << destination.c_step;
        return false;
    }

    int ys, uvs;
    const Planes scaled = CropScale(source, crop, output, scratch, &ys, &uvs);
    libyuv::CopyPlane(scaled.y, ys, destination.y, destination.y_stride, output.width,
                      output.height);
    if (destination.cr == destination.cb + 1) {  // NV12
        libyuv::MergeUVPlane(scaled.u, uvs, scaled.v, uvs, destination.cb, destination.c_stride, cw,
                             ch);
    } else if (destination.cb == destination.cr + 1) {  // NV21
        libyuv::MergeUVPlane(scaled.v, uvs, scaled.u, uvs, destination.cr, destination.c_stride, cw,
                             ch);
    } else {
        LOG(ERROR) << "unsupported interleaved chroma layout";
        return false;
    }
    return true;
}

bool ScaleToRgba(const I420Image& source, const Rect& crop, Size output,
                 const RgbaDestination& destination, I420Image* scratch) {
    int ys, uvs;
    const Planes scaled = CropScale(source, crop, output, scratch, &ys, &uvs);
    // libyuv's ABGR is R, G, B, A in memory.
    const int result = source.full_range
                               ? libyuv::J420ToABGR(scaled.y, ys, scaled.u, uvs, scaled.v, uvs,
                                                    destination.data, destination.stride_bytes,
                                                    output.width, output.height)
                               : libyuv::I420ToABGR(scaled.y, ys, scaled.u, uvs, scaled.v, uvs,
                                                    destination.data, destination.stride_bytes,
                                                    output.width, output.height);
    return result == 0;
}

void FillBlack(I420Image* image) {
    const int cw = (image->width() + 1) / 2;
    const int ch = (image->height() + 1) / 2;
    libyuv::SetPlane(image->y(), image->y_stride(), image->width(), image->height(), 0);
    libyuv::SetPlane(image->u(), image->uv_stride(), cw, ch, 128);
    libyuv::SetPlane(image->v(), image->uv_stride(), cw, ch, 128);
    image->full_range = true;
}

int MeanLuma(const I420Image& image) {
    // Every 8th pixel of every 8th row is plenty for an average.
    constexpr int kStep = 8;
    int64_t sum = 0;
    int64_t count = 0;
    for (int y = 0; y < image.height(); y += kStep) {
        const uint8_t* row = image.y() + static_cast<size_t>(y) * image.y_stride();
        for (int x = 0; x < image.width(); x += kStep) sum += row[x];
        count += (image.width() + kStep - 1) / kStep;
    }
    if (count == 0) return 0;
    const int mean = static_cast<int>(sum / count);
    // Video range: 16-235.
    return image.full_range ? mean : std::clamp((mean - 16) * 255 / 219, 0, 255);
}

}  // namespace aidl::android::hardware::camera::mainline
