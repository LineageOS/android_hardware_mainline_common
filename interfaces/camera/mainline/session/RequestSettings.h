/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "convert/Image.h"
#include "device/CameraDescription.h"
#include "utils/Metadata.h"

namespace aidl::android::hardware::camera::mainline {

// The parts of a request's settings the session acts on.
struct RequestSettings {
    // AE target fps range, validated against the available ranges.
    std::array<int32_t, 2> fps_range = {0, 0};
    bool ae_lock = false;
    bool awb_lock = false;
    // BLACK or SOLID_COLOR test pattern: output black frames.
    bool black = false;

    // For the flash: ANDROID_CONTROL_AE_MODE, ANDROID_FLASH_MODE,
    // ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, and whether the capture intent
    // is STILL_CAPTURE.
    uint8_t ae_mode = 0;
    uint8_t flash_mode = 0;
    uint8_t precapture_trigger = 0;
    bool still_capture = false;

    // Field of view to output, in active array coordinates.
    Rect region;
    // What the result reports for it.
    std::array<int32_t, 4> result_crop_region = {0, 0, 0, 0};
    float result_zoom_ratio = 1.0f;
};

RequestSettings ParseSettings(const Metadata& settings, const CameraDescription& description);

// Maps a rectangle in active array coordinates to a capture frame of
// `capture`, which covers the full active array.
Rect ToCaptureCoordinates(const Rect& region, Size active_array, Size capture);

// Picks the frame interval (from `intervals`, shortest first) for a target
// fps range: the fastest one within `max_fps`, or the slowest if all are
// faster.
Fraction ChooseFrameInterval(const std::vector<Fraction>& intervals, int32_t max_fps);

// Result metadata of a request: its settings with the values actually used,
// plus the dynamic keys.
Metadata BuildResult(const Metadata& settings, const RequestSettings& parsed, int64_t timestamp_ns,
                     uint8_t pipeline_depth);

}  // namespace aidl::android::hardware::camera::mainline
