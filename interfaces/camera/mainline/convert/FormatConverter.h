/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "convert/Image.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// Converts a captured frame in any format ClassifyPixelFormat() calls
// processed into `out`, resized to the frame size. Returns false (and logs)
// when the frame is short, corrupt or in an unexpected layout.
bool ConvertToI420(const CaptureFormat& format, const CapturedFrame& frame, I420Image* out);

// The largest rectangle with the aspect ratio of `output` centered in
// `region`, with even coordinates and size (as needed for 4:2:0 chroma).
Rect CenterCropToAspect(const Rect& region, Size output);

// Crops `crop` out of `source` and scales it to `output`. `scratch` is used
// for intermediate images. Return false for a layout they can not write.
bool ScaleToYuv(const I420Image& source, const Rect& crop, Size output,
                const YuvDestination& destination, I420Image* scratch);
bool ScaleToRgba(const I420Image& source, const Rect& crop, Size output,
                 const RgbaDestination& destination, I420Image* scratch);
// `destination` must already have the output size.
void ScaleToI420(const I420Image& source, const Rect& crop, I420Image* destination);

// Solid black, for the BLACK / SOLID_COLOR test patterns (camera privacy).
void FillBlack(Size output, const YuvDestination& destination);
void FillBlack(Size output, const RgbaDestination& destination);

}  // namespace aidl::android::hardware::camera::mainline
