/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "session/RequestSettings.h"

#include <algorithm>
#include <cmath>

#include <system/camera_metadata.h>

#include "device/RequestTemplates.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

// Zoom ratios this close to 1 are no zoom.
constexpr float kZoomEpsilon = 1e-4f;

Rect Centered(Size array, int32_t width, int32_t height) {
    return {(array.width - width) / 2, (array.height - height) / 2, width, height};
}

}  // namespace

RequestSettings ParseSettings(const Metadata& settings, const CameraDescription& description) {
    RequestSettings parsed;

    const auto fps = settings.GetI32s(ANDROID_CONTROL_AE_TARGET_FPS_RANGE);
    const auto& ranges = description.fps_ranges();
    if (fps.size() == 2 && std::find(ranges.begin(), ranges.end(),
                                     std::array<int32_t, 2>{fps[0], fps[1]}) != ranges.end()) {
        parsed.fps_range = {fps[0], fps[1]};
    } else {
        parsed.fps_range = DefaultFpsRange(description, /*fixed=*/false);
    }

    parsed.ae_lock = settings.GetU8(ANDROID_CONTROL_AE_LOCK) == ANDROID_CONTROL_AE_LOCK_ON;
    parsed.awb_lock = settings.GetU8(ANDROID_CONTROL_AWB_LOCK) == ANDROID_CONTROL_AWB_LOCK_ON;

    const auto pattern = settings.GetI32(ANDROID_SENSOR_TEST_PATTERN_MODE);
    parsed.black = pattern == ANDROID_SENSOR_TEST_PATTERN_MODE_SOLID_COLOR ||
                   pattern == ANDROID_SENSOR_TEST_PATTERN_MODE_BLACK;

    const Size array = description.active_array();
    const float max_zoom = description.max_zoom();
    const float zoom = std::clamp(settings.GetFloat(ANDROID_CONTROL_ZOOM_RATIO).value_or(1.0f),
                                  1.0f, max_zoom);
    if (zoom > 1.0f + kZoomEpsilon) {
        // Zoom ratio takes over; the crop region then stays the full array.
        parsed.region = Centered(array, static_cast<int32_t>(array.width / zoom),
                                 static_cast<int32_t>(array.height / zoom));
        parsed.result_crop_region = {0, 0, array.width, array.height};
        parsed.result_zoom_ratio = zoom;
        return parsed;
    }

    Rect crop = {0, 0, array.width, array.height};
    const auto region = settings.GetI32s(ANDROID_SCALER_CROP_REGION);
    if (region.size() == 4 && region[2] > 0 && region[3] > 0) {
        crop = {region[0], region[1], region[2], region[3]};
    }
    // Keep it inside the array and no smaller than the maximum zoom allows.
    const int32_t min_width = static_cast<int32_t>(std::ceil(array.width / max_zoom));
    const int32_t min_height = static_cast<int32_t>(std::ceil(array.height / max_zoom));
    crop.width = std::clamp(crop.width, min_width, array.width);
    crop.height = std::clamp(crop.height, min_height, array.height);
    crop.x = std::clamp(crop.x, 0, array.width - crop.width);
    crop.y = std::clamp(crop.y, 0, array.height - crop.height);
    parsed.region = crop;
    parsed.result_crop_region = {crop.x, crop.y, crop.width, crop.height};
    parsed.result_zoom_ratio = 1.0f;
    return parsed;
}

Rect ToCaptureCoordinates(const Rect& region, Size active_array, Size capture) {
    if (active_array.width <= 0 || active_array.height <= 0)
        return {0, 0, capture.width, capture.height};
    auto scale = [](int32_t value, int32_t to, int32_t from) {
        return static_cast<int32_t>(static_cast<int64_t>(value) * to / from);
    };
    Rect result = {scale(region.x, capture.width, active_array.width),
                   scale(region.y, capture.height, active_array.height),
                   scale(region.width, capture.width, active_array.width),
                   scale(region.height, capture.height, active_array.height)};
    result.width = std::clamp(result.width, 2, capture.width);
    result.height = std::clamp(result.height, 2, capture.height);
    result.x = std::clamp(result.x, 0, capture.width - result.width);
    result.y = std::clamp(result.y, 0, capture.height - result.height);
    return result;
}

Fraction ChooseFrameInterval(const std::vector<Fraction>& intervals, int32_t max_fps) {
    if (intervals.empty()) return {1, 30};
    for (const auto& interval : intervals) {
        const int64_t ns = interval.ToNanoseconds();
        if (ns <= 0) continue;
        // Allow a little slack, e.g. 1001/30000 for 30 fps.
        if (1e9 / static_cast<double>(ns) <= max_fps + 0.5) return interval;
    }
    return intervals.back();
}

Metadata BuildResult(const Metadata& settings, const RequestSettings& parsed, int64_t timestamp_ns,
                     uint8_t pipeline_depth) {
    Metadata result = settings;
    result.Set(ANDROID_SCALER_CROP_REGION, std::vector<int32_t>(parsed.result_crop_region.begin(),
                                                                parsed.result_crop_region.end()));
    result.SetFloat(ANDROID_CONTROL_ZOOM_RATIO, parsed.result_zoom_ratio);
    result.Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
               std::vector<int32_t>{parsed.fps_range[0], parsed.fps_range[1]});
    result.SetI64(ANDROID_SENSOR_TIMESTAMP, timestamp_ns);
    result.SetU8(ANDROID_REQUEST_PIPELINE_DEPTH, pipeline_depth);

    result.SetU8(ANDROID_CONTROL_AE_STATE, parsed.ae_lock ? ANDROID_CONTROL_AE_STATE_LOCKED
                                                          : ANDROID_CONTROL_AE_STATE_CONVERGED);
    result.SetU8(ANDROID_CONTROL_AWB_STATE, parsed.awb_lock ? ANDROID_CONTROL_AWB_STATE_LOCKED
                                                            : ANDROID_CONTROL_AWB_STATE_CONVERGED);
    result.SetU8(ANDROID_CONTROL_AF_STATE, ANDROID_CONTROL_AF_STATE_INACTIVE);
    result.SetU8(ANDROID_FLASH_STATE, ANDROID_FLASH_STATE_UNAVAILABLE);
    result.SetU8(ANDROID_LENS_STATE, ANDROID_LENS_STATE_STATIONARY);
    result.SetU8(ANDROID_STATISTICS_SCENE_FLICKER, ANDROID_STATISTICS_SCENE_FLICKER_NONE);
    return result;
}

}  // namespace aidl::android::hardware::camera::mainline
