/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "device/RequestTemplates.h"

#include <algorithm>

#include <system/camera_metadata.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

using device::RequestTemplate;

// Frame rate a preview should not exceed by default.
constexpr int32_t kPreviewFrameRate = 30;

}  // namespace

std::array<int32_t, 2> DefaultFpsRange(const CameraDescription& description, bool fixed) {
    const auto& ranges = description.fps_ranges();
    if (ranges.empty()) return {kPreviewFrameRate, kPreviewFrameRate};
    int32_t max = 0;
    for (const auto& range : ranges) {
        if (range[1] <= kPreviewFrameRate) max = std::max(max, range[1]);
    }
    if (max == 0) {
        max = ranges.front()[1];
        for (const auto& range : ranges) max = std::min(max, range[1]);
    }
    std::array<int32_t, 2> result = {max, max};
    if (!fixed) {
        for (const auto& range : ranges) {
            if (range[1] == max) result[0] = std::min(result[0], range[0]);
        }
    }
    return result;
}

std::optional<Metadata> BuildRequestTemplate(const CameraDescription& description,
                                             RequestTemplate type) {
    uint8_t intent;
    bool video = false;
    switch (type) {
        case RequestTemplate::PREVIEW:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
            break;
        case RequestTemplate::STILL_CAPTURE:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
            break;
        case RequestTemplate::VIDEO_RECORD:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
            video = true;
            break;
        case RequestTemplate::VIDEO_SNAPSHOT:
            intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
            video = true;
            break;
        default:
            return std::nullopt;
    }

    Metadata m;
    m.SetU8(ANDROID_CONTROL_CAPTURE_INTENT, intent);
    m.SetU8(ANDROID_CONTROL_MODE, ANDROID_CONTROL_MODE_AUTO);
    m.SetU8(ANDROID_CONTROL_SCENE_MODE, ANDROID_CONTROL_SCENE_MODE_DISABLED);
    m.SetU8(ANDROID_CONTROL_EFFECT_MODE, ANDROID_CONTROL_EFFECT_MODE_OFF);
    m.SetU8(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF);

    m.SetU8(ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_MODE_ON);
    m.SetU8(ANDROID_CONTROL_AE_LOCK, ANDROID_CONTROL_AE_LOCK_OFF);
    m.SetU8(ANDROID_CONTROL_AE_ANTIBANDING_MODE, ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO);
    m.SetI32(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, 0);
    m.SetU8(ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE);
    const auto fps = DefaultFpsRange(description, video);
    m.Set(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, std::vector<int32_t>{fps[0], fps[1]});

    m.SetU8(ANDROID_CONTROL_AF_MODE, ANDROID_CONTROL_AF_MODE_OFF);
    m.SetU8(ANDROID_CONTROL_AF_TRIGGER, ANDROID_CONTROL_AF_TRIGGER_IDLE);

    m.SetU8(ANDROID_CONTROL_AWB_MODE, ANDROID_CONTROL_AWB_MODE_AUTO);
    m.SetU8(ANDROID_CONTROL_AWB_LOCK, ANDROID_CONTROL_AWB_LOCK_OFF);

    m.SetFloat(ANDROID_CONTROL_ZOOM_RATIO, 1.0f);
    const Size array = description.active_array();
    m.Set(ANDROID_SCALER_CROP_REGION, std::vector<int32_t>{0, 0, array.width, array.height});

    m.SetU8(ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
            ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST);
    m.SetU8(ANDROID_EDGE_MODE, ANDROID_EDGE_MODE_OFF);
    m.SetU8(ANDROID_HOT_PIXEL_MODE, ANDROID_HOT_PIXEL_MODE_OFF);
    m.SetU8(ANDROID_NOISE_REDUCTION_MODE, ANDROID_NOISE_REDUCTION_MODE_OFF);

    m.SetU8(ANDROID_FLASH_MODE, ANDROID_FLASH_MODE_OFF);

    m.SetU8(ANDROID_JPEG_QUALITY, 90);
    m.SetU8(ANDROID_JPEG_THUMBNAIL_QUALITY, 90);
    m.Set(ANDROID_JPEG_THUMBNAIL_SIZE, std::vector<int32_t>{240, 180});
    m.SetI32(ANDROID_JPEG_ORIENTATION, 0);

    m.SetFloat(ANDROID_LENS_APERTURE, description.aperture());
    m.SetFloat(ANDROID_LENS_FOCAL_LENGTH, description.focal_length());
    m.SetU8(ANDROID_LENS_OPTICAL_STABILIZATION_MODE, ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF);

    m.SetI32(ANDROID_SENSOR_TEST_PATTERN_MODE, ANDROID_SENSOR_TEST_PATTERN_MODE_OFF);

    m.SetU8(ANDROID_STATISTICS_FACE_DETECT_MODE, ANDROID_STATISTICS_FACE_DETECT_MODE_OFF);
    m.SetU8(ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE, ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF);
    m.SetU8(ANDROID_STATISTICS_LENS_SHADING_MAP_MODE, ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF);
    return m;
}

}  // namespace aidl::android::hardware::camera::mainline
