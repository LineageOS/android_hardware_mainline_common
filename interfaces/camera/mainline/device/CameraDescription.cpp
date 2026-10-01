/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Description"

#include "device/CameraDescription.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <aidl/android/hardware/camera/metadata/RequestAvailableColorSpaceProfilesMap.h>
#include <aidl/android/hardware/camera/metadata/RequestAvailableDynamicRangeProfilesMap.h>
#include <aidl/android/hardware/camera/metadata/ScalerAvailableStreamUseCases.h>
#include <aidl/android/hardware/camera/metadata/SensorPixelMode.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <system/camera_metadata.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::aidl::android::hardware::graphics::common::Dataspace;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using ::android::base::StringPrintf;

// Largest digital zoom (center crop and upscale).
constexpr float kMaxDigitalZoom = 4.0f;

// V4L2 knows nothing about optics. These nominal values, a 3.6 mm wide
// sensor behind a 70 degree (horizontal) lens at f/2.0, are typical for
// webcams and phone front cameras; they only feed field of view
// calculations of apps.
constexpr float kNominalSensorWidthMm = 3.6f;
constexpr float kNominalHorizontalFovDegrees = 70.0f;
constexpr float kNominalAperture = 2.0f;

// Time the JPEG encoder is assumed to take per pixel, for the stall
// duration of JPEG streams.
constexpr int64_t kJpegStallNsPerPixel = 40;

// Room for the APP1 segment (EXIF and thumbnail) on top of the image data.
constexpr int32_t kJpegHeaderSize = 256 * 1024;

// Frame rate below which the lower end of the fps ranges does not go.
constexpr int32_t kMinRangeFrameRate = 15;

constexpr int32_t kThumbnailSizes[] = {0,   0,   176, 144, 240, 144, 256,
                                       144, 240, 160, 256, 154, 240, 180};

// Requests keys the session handles.
const std::vector<int32_t> kRequestKeys = {
        ANDROID_COLOR_CORRECTION_ABERRATION_MODE,
        ANDROID_CONTROL_AE_ANTIBANDING_MODE,
        ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION,
        ANDROID_CONTROL_AE_LOCK,
        ANDROID_CONTROL_AE_MODE,
        ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
        ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
        ANDROID_CONTROL_AF_MODE,
        ANDROID_CONTROL_AF_TRIGGER,
        ANDROID_CONTROL_AWB_LOCK,
        ANDROID_CONTROL_AWB_MODE,
        ANDROID_CONTROL_CAPTURE_INTENT,
        ANDROID_CONTROL_EFFECT_MODE,
        ANDROID_CONTROL_MODE,
        ANDROID_CONTROL_SCENE_MODE,
        ANDROID_CONTROL_VIDEO_STABILIZATION_MODE,
        ANDROID_CONTROL_ZOOM_RATIO,
        ANDROID_EDGE_MODE,
        ANDROID_FLASH_MODE,
        ANDROID_HOT_PIXEL_MODE,
        ANDROID_JPEG_GPS_COORDINATES,
        ANDROID_JPEG_GPS_PROCESSING_METHOD,
        ANDROID_JPEG_GPS_TIMESTAMP,
        ANDROID_JPEG_ORIENTATION,
        ANDROID_JPEG_QUALITY,
        ANDROID_JPEG_THUMBNAIL_QUALITY,
        ANDROID_JPEG_THUMBNAIL_SIZE,
        ANDROID_LENS_APERTURE,
        ANDROID_LENS_FOCAL_LENGTH,
        ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
        ANDROID_NOISE_REDUCTION_MODE,
        ANDROID_SCALER_CROP_REGION,
        ANDROID_SENSOR_TEST_PATTERN_MODE,
        ANDROID_STATISTICS_FACE_DETECT_MODE,
        ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE,
        ANDROID_STATISTICS_LENS_SHADING_MAP_MODE,
};

// Result keys the session reports, in addition to the request keys.
const std::vector<int32_t> kResultOnlyKeys = {
        ANDROID_CONTROL_AE_STATE,  ANDROID_CONTROL_AF_STATE,
        ANDROID_CONTROL_AWB_STATE, ANDROID_FLASH_STATE,
        ANDROID_LENS_STATE,        ANDROID_REQUEST_PIPELINE_DEPTH,
        ANDROID_SENSOR_TIMESTAMP,  ANDROID_STATISTICS_SCENE_FLICKER,
};

bool IsJpegDataspace(Dataspace dataspace) {
    // The framework uses JFIF; VTS also configures JPEG streams with UNKNOWN.
    return dataspace == Dataspace::JFIF || dataspace == Dataspace::UNKNOWN;
}

}  // namespace

std::shared_ptr<const CameraDescription> CameraDescription::Create(const CameraCandidate& candidate,
                                                                   int32_t flash_levels) {
    std::shared_ptr<CameraDescription> description(new CameraDescription(candidate, flash_levels));
    if (description->planner_.OutputSizes().empty()) {
        LOG(ERROR) << candidate.key << ": no usable output size";
        return nullptr;
    }
    description->BuildCharacteristics();
    return description;
}

CameraDescription::CameraDescription(const CameraCandidate& candidate, int32_t flash_levels)
    : candidate_(candidate), flash_levels_(flash_levels), planner_(candidate.formats) {}

void CameraDescription::BuildCharacteristics() {
    const Size array = planner_.MaxSize();
    max_zoom_ = kMaxDigitalZoom;
    jpeg_max_size_ = static_cast<int32_t>(
            std::min<int64_t>(array.Area() * 3 / 2 + kJpegHeaderSize, INT32_MAX));

    // Fixed ranges for every frame rate, plus variable ones reaching down to
    // kMinRangeFrameRate.
    std::set<std::array<int32_t, 2>> ranges;
    for (const int32_t rate : planner_.FrameRates()) {
        ranges.insert({rate, rate});
        if (rate > kMinRangeFrameRate) ranges.insert({kMinRangeFrameRate, rate});
    }
    fps_ranges_.assign(ranges.begin(), ranges.end());
    std::sort(fps_ranges_.begin(), fps_ranges_.end(), [](const auto& a, const auto& b) {
        return a[1] != b[1] ? a[1] < b[1] : a[0] < b[0];
    });
    int32_t min_rate = INT32_MAX;
    for (const auto& range : fps_ranges_) min_rate = std::min(min_rate, range[0]);

    const float physical_width = kNominalSensorWidthMm;
    const float physical_height = kNominalSensorWidthMm * static_cast<float>(array.height) /
                                  static_cast<float>(array.width);
    focal_length_ =
            physical_width / (2.0f * std::tan(kNominalHorizontalFovDegrees / 2.0f * M_PI / 180.0f));
    aperture_ = kNominalAperture;

    Metadata& m = characteristics_;

    // android.colorCorrection: nothing to correct, FAST is the same as OFF.
    m.Set(ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
          std::vector<uint8_t>{ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF,
                               ANDROID_COLOR_CORRECTION_ABERRATION_MODE_FAST});

    // android.control
    m.Set(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
          std::vector<uint8_t>{ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO});
    std::vector<uint8_t> ae_modes = {ANDROID_CONTROL_AE_MODE_ON};
    if (has_flash()) {
        ae_modes.push_back(ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH);
        ae_modes.push_back(ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH);
    }
    m.Set(ANDROID_CONTROL_AE_AVAILABLE_MODES, ae_modes);
    std::vector<int32_t> fps_ranges;
    for (const auto& range : fps_ranges_) {
        fps_ranges.push_back(range[0]);
        fps_ranges.push_back(range[1]);
    }
    m.Set(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fps_ranges);
    m.Set(ANDROID_CONTROL_AE_COMPENSATION_RANGE, std::vector<int32_t>{0, 0});
    m.Set(ANDROID_CONTROL_AE_COMPENSATION_STEP, std::vector<camera_metadata_rational_t>{{1, 2}});
    m.SetU8(ANDROID_CONTROL_AE_LOCK_AVAILABLE, ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE);
    m.Set(ANDROID_CONTROL_AF_AVAILABLE_MODES, std::vector<uint8_t>{ANDROID_CONTROL_AF_MODE_OFF});
    m.Set(ANDROID_CONTROL_AVAILABLE_EFFECTS, std::vector<uint8_t>{ANDROID_CONTROL_EFFECT_MODE_OFF});
    m.Set(ANDROID_CONTROL_AVAILABLE_MODES, std::vector<uint8_t>{ANDROID_CONTROL_MODE_AUTO});
    m.Set(ANDROID_CONTROL_AVAILABLE_SCENE_MODES,
          std::vector<uint8_t>{ANDROID_CONTROL_SCENE_MODE_DISABLED});
    m.Set(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
          std::vector<uint8_t>{ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF});
    m.Set(ANDROID_CONTROL_AWB_AVAILABLE_MODES, std::vector<uint8_t>{ANDROID_CONTROL_AWB_MODE_AUTO});
    m.SetU8(ANDROID_CONTROL_AWB_LOCK_AVAILABLE, ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE);
    m.Set(ANDROID_CONTROL_MAX_REGIONS, std::vector<int32_t>{0, 0, 0});
    m.Set(ANDROID_CONTROL_ZOOM_RATIO_RANGE, std::vector<float>{1.0f, max_zoom_});

    // android.edge, android.hotPixel, android.noiseReduction, android.shading
    m.Set(ANDROID_EDGE_AVAILABLE_EDGE_MODES, std::vector<uint8_t>{ANDROID_EDGE_MODE_OFF});
    m.Set(ANDROID_HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES,
          std::vector<uint8_t>{ANDROID_HOT_PIXEL_MODE_OFF});
    m.Set(ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
          std::vector<uint8_t>{ANDROID_NOISE_REDUCTION_MODE_OFF});
    m.Set(ANDROID_SHADING_AVAILABLE_MODES, std::vector<uint8_t>{ANDROID_SHADING_MODE_OFF});

    // android.flash: LEDs at torch brightness, also for flash captures.
    m.SetU8(ANDROID_FLASH_INFO_AVAILABLE,
            has_flash() ? ANDROID_FLASH_INFO_AVAILABLE_TRUE : ANDROID_FLASH_INFO_AVAILABLE_FALSE);
    if (flash_levels_ > 1) {
        // Torch strength control.
        m.SetI32(ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL, flash_levels_);
        m.SetI32(ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL, flash_levels_);
    }

    // android.info
    m.SetU8(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL,
            internal() ? ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED
                       : ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL);

    // android.jpeg
    m.Set(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES,
          std::vector<int32_t>(std::begin(kThumbnailSizes), std::end(kThumbnailSizes)));
    m.SetI32(ANDROID_JPEG_MAX_SIZE, jpeg_max_size_);

    // android.lens
    m.SetU8(ANDROID_LENS_FACING, !internal() ? ANDROID_LENS_FACING_EXTERNAL
                                 : candidate_.facing == Facing::kFront ? ANDROID_LENS_FACING_FRONT
                                                                       : ANDROID_LENS_FACING_BACK);
    m.Set(ANDROID_LENS_INFO_AVAILABLE_APERTURES, std::vector<float>{aperture_});
    m.Set(ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES, std::vector<float>{0.0f});
    m.Set(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, std::vector<float>{focal_length_});
    m.Set(ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
          std::vector<uint8_t>{ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF});
    m.SetU8(ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
            ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED);
    // Fixed focus.
    m.SetFloat(ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, 0.0f);
    m.SetFloat(ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, 0.0f);

    // android.request
    m.Set(ANDROID_REQUEST_AVAILABLE_CAPABILITIES,
          std::vector<uint8_t>{ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE});
    m.SetI32(ANDROID_REQUEST_MAX_NUM_INPUT_STREAMS, 0);
    m.Set(ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
          std::vector<int32_t>{0, kMaxProcessedStreams, kMaxStallingStreams});
    m.SetI32(ANDROID_REQUEST_PARTIAL_RESULT_COUNT, 1);
    m.SetU8(ANDROID_REQUEST_PIPELINE_MAX_DEPTH, kPipelineMaxDepth);
    m.Set(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, kRequestKeys);
    std::vector<int32_t> result_keys = kRequestKeys;
    result_keys.insert(result_keys.end(), kResultOnlyKeys.begin(), kResultOnlyKeys.end());
    m.Set(ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, result_keys);

    // android.scaler
    m.SetFloat(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, max_zoom_);
    m.SetU8(ANDROID_SCALER_CROPPING_TYPE, ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY);
    std::vector<int32_t> configurations;
    std::vector<int64_t> min_durations;
    std::vector<int64_t> stall_durations;
    std::vector<PixelFormat> formats = {PixelFormat::IMPLEMENTATION_DEFINED,
                                        PixelFormat::YCBCR_420_888, PixelFormat::BLOB};
    if (candidate_.advertise_rgb) formats.push_back(PixelFormat::RGBA_8888);
    for (const auto& output : planner_.OutputSizes()) {
        for (const PixelFormat format : formats) {
            const auto f = static_cast<int32_t>(format);
            configurations.insert(configurations.end(),
                                  {f, output.size.width, output.size.height,
                                   ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT});
            min_durations.insert(min_durations.end(), {f, output.size.width, output.size.height,
                                                       output.min_frame_duration_ns});
            const int64_t stall =
                    format == PixelFormat::BLOB ? output.size.Area() * kJpegStallNsPerPixel : 0;
            stall_durations.insert(stall_durations.end(),
                                   {f, output.size.width, output.size.height, stall});
        }
    }
    m.Set(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, configurations);
    m.Set(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, min_durations);
    m.Set(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, stall_durations);

    // android.sensor
    const std::vector<int32_t> active_array = {0, 0, array.width, array.height};
    m.Set(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, active_array);
    m.Set(ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, active_array);
    m.Set(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, std::vector<int32_t>{array.width, array.height});
    m.Set(ANDROID_SENSOR_INFO_PHYSICAL_SIZE, std::vector<float>{physical_width, physical_height});
    m.SetI64(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION, 1'000'000'000LL / std::max(min_rate, 1));
    // Timestamps are converted to CLOCK_BOOTTIME.
    m.SetU8(ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME);
    m.SetI32(ANDROID_SENSOR_ORIENTATION, internal() ? candidate_.rotation : 0);
    // SOLID_COLOR and BLACK let the framework mute the camera (sensor
    // privacy) by having the HAL output black frames.
    m.Set(ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
          std::vector<int32_t>{ANDROID_SENSOR_TEST_PATTERN_MODE_OFF,
                               ANDROID_SENSOR_TEST_PATTERN_MODE_SOLID_COLOR,
                               ANDROID_SENSOR_TEST_PATTERN_MODE_BLACK});
    m.SetU8(ANDROID_SENSOR_READOUT_TIMESTAMP, ANDROID_SENSOR_READOUT_TIMESTAMP_NOT_SUPPORTED);

    // android.statistics
    m.Set(ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
          std::vector<uint8_t>{ANDROID_STATISTICS_FACE_DETECT_MODE_OFF});
    m.Set(ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
          std::vector<uint8_t>{ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF});
    m.Set(ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
          std::vector<uint8_t>{ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF});
    m.SetI32(ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, 0);

    // android.sync
    m.SetI32(ANDROID_SYNC_MAX_LATENCY, ANDROID_SYNC_MAX_LATENCY_UNKNOWN);

    // Every key set above, and this one.
    std::vector<int32_t> keys = m.Tags();
    keys.push_back(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    m.Set(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, keys);

    LOG(INFO) << candidate_.key << ": " << planner_.OutputSizes().size()
              << " output size(s), largest " << array.width << "x" << array.height << ", "
              << fps_ranges_.size() << " fps range(s)";
}

std::optional<CaptureMode> CameraDescription::PlanStreams(const device::StreamConfiguration& config,
                                                          std::string* why) const {
    using device::StreamConfigurationMode;
    using device::StreamRotation;
    using device::StreamType;
    using metadata::RequestAvailableColorSpaceProfilesMap;
    using metadata::RequestAvailableDynamicRangeProfilesMap;
    using metadata::ScalerAvailableStreamUseCases;
    using metadata::SensorPixelMode;

    if (config.operationMode != StreamConfigurationMode::NORMAL_MODE) {
        *why = "unsupported operation mode " + toString(config.operationMode);
        return std::nullopt;
    }
    if (config.streams.empty()) {
        *why = "no streams";
        return std::nullopt;
    }

    int processed = 0;
    int stalling = 0;
    std::vector<Size> sizes;
    for (const auto& stream : config.streams) {
        const std::string what = StringPrintf("stream %d (%dx%d %s)", stream.id, stream.width,
                                              stream.height, toString(stream.format).c_str());
        if (stream.streamType != StreamType::OUTPUT) {
            *why = what + ": input streams are not supported";
            return std::nullopt;
        }
        if (stream.rotation != StreamRotation::ROTATION_0) {
            *why = what + ": rotation is not supported";
            return std::nullopt;
        }
        if (!stream.physicalCameraId.empty()) {
            *why = what + ": not a logical camera";
            return std::nullopt;
        }
        for (const auto mode : stream.sensorPixelModesUsed) {
            if (mode != SensorPixelMode::ANDROID_SENSOR_PIXEL_MODE_DEFAULT) {
                *why = what + ": unsupported sensor pixel mode";
                return std::nullopt;
            }
        }
        if (stream.dynamicRangeProfile !=
            RequestAvailableDynamicRangeProfilesMap::
                    ANDROID_REQUEST_AVAILABLE_DYNAMIC_RANGE_PROFILES_MAP_STANDARD) {
            *why = what + ": unsupported dynamic range profile";
            return std::nullopt;
        }
        if (stream.useCase !=
            ScalerAvailableStreamUseCases::ANDROID_SCALER_AVAILABLE_STREAM_USE_CASES_DEFAULT) {
            *why = what + ": unsupported stream use case";
            return std::nullopt;
        }
        if (stream.colorSpace !=
                    static_cast<int32_t>(
                            RequestAvailableColorSpaceProfilesMap::
                                    ANDROID_REQUEST_AVAILABLE_COLOR_SPACE_PROFILES_MAP_UNSPECIFIED) &&
            stream.colorSpace !=
                    static_cast<int32_t>(
                            RequestAvailableColorSpaceProfilesMap::
                                    ANDROID_REQUEST_AVAILABLE_COLOR_SPACE_PROFILES_MAP_SRGB)) {
            *why = what + ": unsupported color space";
            return std::nullopt;
        }

        switch (stream.format) {
            case PixelFormat::BLOB:
                if (!IsJpegDataspace(stream.dataSpace)) {
                    *why = what + ": unsupported data space " + toString(stream.dataSpace);
                    return std::nullopt;
                }
                ++stalling;
                break;
            case PixelFormat::IMPLEMENTATION_DEFINED:
            case PixelFormat::YCBCR_420_888:
                ++processed;
                break;
            case PixelFormat::RGBA_8888:
                if (!candidate_.advertise_rgb) {
                    *why = what + ": unsupported format";
                    return std::nullopt;
                }
                ++processed;
                break;
            default:
                *why = what + ": unsupported format";
                return std::nullopt;
        }

        const Size size = {stream.width, stream.height};
        if (!planner_.IsOutputSize(size)) {
            *why = what + ": unsupported size";
            return std::nullopt;
        }
        sizes.push_back(size);
    }
    if (processed > kMaxProcessedStreams || stalling > kMaxStallingStreams) {
        *why = StringPrintf("too many streams: %d processed, %d stalling", processed, stalling);
        return std::nullopt;
    }

    auto mode = planner_.Plan(sizes);
    if (!mode.has_value()) *why = "no capture mode covers all streams";
    return mode;
}

}  // namespace aidl::android::hardware::camera::mainline
