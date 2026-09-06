/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "libsensors_common/SensorTypes.h"

#include <android-base/strings.h>

#include <cmath>

namespace aidl::android::hardware::sensors::mainline {

using ::aidl::android::hardware::sensors::SensorInfo;
using ::aidl::android::hardware::sensors::SensorType;

namespace {

constexpr int32_t kOneSecondUs = 1000 * 1000;
constexpr float kPi = 3.14159265358979f;

// Defaults are conservative estimates typical for mobile sensors; they are
// only used when the hardware does not report better values and can always be
// overridden through the configuration.
const SensorTypeTraits kTraits[] = {
        {SensorType::ACCELEROMETER, "Accelerometer", "accel", ReportingMode::kContinuous, true,
         4.0f * 9.80665f, 0.001f, 0.15f, 10000, kOneSecondUs, false},
        {SensorType::MAGNETIC_FIELD, "Magnetometer", "magn", ReportingMode::kContinuous, true,
         4900.0f, 0.1f, 0.5f, 20000, kOneSecondUs, false},
        {SensorType::ORIENTATION, "Orientation", "orientation", ReportingMode::kContinuous, true,
         360.0f, 0.1f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::GYROSCOPE, "Gyroscope", "gyro", ReportingMode::kContinuous, true,
         2000.0f * kPi / 180.0f, 0.001f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::LIGHT, "Light", "light", ReportingMode::kOnChange, false, 10000.0f, 1.0f, 0.1f,
         0, kOneSecondUs, false},
        {SensorType::PRESSURE, "Pressure", "pressure", ReportingMode::kContinuous, false, 1100.0f,
         0.01f, 0.05f, 100000, kOneSecondUs, false},
        {SensorType::PROXIMITY, "Proximity", "proximity", ReportingMode::kOnChange, false, 5.0f,
         5.0f, 0.1f, 0, kOneSecondUs, true},
        {SensorType::GRAVITY, "Gravity", "gravity", ReportingMode::kContinuous, true, 9.80665f,
         0.001f, 0.15f, 10000, kOneSecondUs, false},
        {SensorType::LINEAR_ACCELERATION, "Linear Acceleration", "linear_accel",
         ReportingMode::kContinuous, true, 4.0f * 9.80665f, 0.001f, 0.15f, 10000, kOneSecondUs,
         false},
        {SensorType::ROTATION_VECTOR, "Rotation Vector", "rotation_vector",
         ReportingMode::kContinuous, false, 1.0f, 0.0001f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::RELATIVE_HUMIDITY, "Humidity", "humidity", ReportingMode::kOnChange, false,
         100.0f, 0.1f, 0.05f, 0, kOneSecondUs, false},
        {SensorType::AMBIENT_TEMPERATURE, "Temperature", "temperature", ReportingMode::kOnChange,
         false, 125.0f, 0.01f, 0.05f, 0, kOneSecondUs, false},
        {SensorType::MAGNETIC_FIELD_UNCALIBRATED, "Magnetometer Uncalibrated", "magn_uncal",
         ReportingMode::kContinuous, false, 4900.0f, 0.1f, 0.5f, 20000, kOneSecondUs, false},
        {SensorType::GAME_ROTATION_VECTOR, "Game Rotation Vector", "game_rotation_vector",
         ReportingMode::kContinuous, false, 1.0f, 0.0001f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::GYROSCOPE_UNCALIBRATED, "Gyroscope Uncalibrated", "gyro_uncal",
         ReportingMode::kContinuous, false, 2000.0f * kPi / 180.0f, 0.001f, 1.0f, 10000,
         kOneSecondUs, false},
        {SensorType::SIGNIFICANT_MOTION, "Significant Motion", "significant_motion",
         ReportingMode::kOneShot, false, 1.0f, 1.0f, 0.1f, -1, 0, true},
        {SensorType::STEP_DETECTOR, "Step Detector", "step_detector", ReportingMode::kSpecial,
         false, 1.0f, 1.0f, 0.1f, 0, 0, false},
        {SensorType::STEP_COUNTER, "Step Counter", "step_counter", ReportingMode::kOnChange, false,
         1.0e6f, 1.0f, 0.1f, 0, kOneSecondUs, false},
        {SensorType::GEOMAGNETIC_ROTATION_VECTOR, "Geomagnetic Rotation Vector",
         "geomagnetic_rotation_vector", ReportingMode::kContinuous, false, 1.0f, 0.0001f, 1.0f,
         10000, kOneSecondUs, false},
        {SensorType::HEART_RATE, "Heart Rate", "heart_rate", ReportingMode::kOnChange, false,
         250.0f, 1.0f, 1.0f, 0, kOneSecondUs, false},
        {SensorType::TILT_DETECTOR, "Tilt Detector", "tilt_detector", ReportingMode::kSpecial,
         false, 1.0f, 1.0f, 0.1f, 0, 0, true},
        {SensorType::WAKE_GESTURE, "Wake Gesture", "wake_gesture", ReportingMode::kOneShot, false,
         1.0f, 1.0f, 0.1f, -1, 0, true},
        {SensorType::GLANCE_GESTURE, "Glance Gesture", "glance_gesture", ReportingMode::kOneShot,
         false, 1.0f, 1.0f, 0.1f, -1, 0, true},
        {SensorType::PICK_UP_GESTURE, "Pick Up Gesture", "pick_up_gesture", ReportingMode::kOneShot,
         false, 1.0f, 1.0f, 0.1f, -1, 0, true},
        {SensorType::WRIST_TILT_GESTURE, "Wrist Tilt Gesture", "wrist_tilt_gesture",
         ReportingMode::kSpecial, false, 1.0f, 1.0f, 0.1f, 0, 0, true},
        {SensorType::DEVICE_ORIENTATION, "Device Orientation", "device_orientation",
         ReportingMode::kOnChange, false, 3.0f, 1.0f, 0.1f, 0, kOneSecondUs, false},
        {SensorType::POSE_6DOF, "Pose 6DOF", "pose_6dof", ReportingMode::kContinuous, false, 1.0f,
         0.0001f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::STATIONARY_DETECT, "Stationary Detect", "stationary_detect",
         ReportingMode::kOneShot, false, 1.0f, 1.0f, 0.1f, -1, 0, true},
        {SensorType::MOTION_DETECT, "Motion Detect", "motion_detect", ReportingMode::kOneShot,
         false, 1.0f, 1.0f, 0.1f, -1, 0, true},
        {SensorType::HEART_BEAT, "Heart Beat", "heart_beat", ReportingMode::kContinuous, false,
         1.0f, 1.0f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::LOW_LATENCY_OFFBODY_DETECT, "Off-body Detect", "offbody_detect",
         ReportingMode::kOnChange, false, 1.0f, 1.0f, 0.1f, 0, kOneSecondUs, true},
        {SensorType::ACCELEROMETER_UNCALIBRATED, "Accelerometer Uncalibrated", "accel_uncal",
         ReportingMode::kContinuous, false, 4.0f * 9.80665f, 0.001f, 0.15f, 10000, kOneSecondUs,
         false},
        {SensorType::HINGE_ANGLE, "Hinge Angle", "hinge_angle", ReportingMode::kOnChange, false,
         360.0f, 1.0f, 0.1f, 0, kOneSecondUs, true},
        {SensorType::HEAD_TRACKER, "Head Tracker", "head_tracker", ReportingMode::kContinuous,
         false, kPi, 0.0001f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::ACCELEROMETER_LIMITED_AXES, "Accelerometer Limited Axes", "accel_limited_axes",
         ReportingMode::kContinuous, false, 4.0f * 9.80665f, 0.001f, 0.15f, 10000, kOneSecondUs,
         false},
        {SensorType::GYROSCOPE_LIMITED_AXES, "Gyroscope Limited Axes", "gyro_limited_axes",
         ReportingMode::kContinuous, false, 2000.0f * kPi / 180.0f, 0.001f, 1.0f, 10000,
         kOneSecondUs, false},
        {SensorType::ACCELEROMETER_LIMITED_AXES_UNCALIBRATED,
         "Accelerometer Limited Axes Uncalibrated", "accel_limited_axes_uncal",
         ReportingMode::kContinuous, false, 4.0f * 9.80665f, 0.001f, 0.15f, 10000, kOneSecondUs,
         false},
        {SensorType::GYROSCOPE_LIMITED_AXES_UNCALIBRATED, "Gyroscope Limited Axes Uncalibrated",
         "gyro_limited_axes_uncal", ReportingMode::kContinuous, false, 2000.0f * kPi / 180.0f,
         0.001f, 1.0f, 10000, kOneSecondUs, false},
        {SensorType::HEADING, "Heading", "heading", ReportingMode::kContinuous, false, 360.0f, 0.1f,
         1.0f, 10000, kOneSecondUs, false},
        {SensorType::MOISTURE_INTRUSION, "Moisture Intrusion", "moisture_intrusion",
         ReportingMode::kOnChange, false, 1.0f, 1.0f, 0.1f, 0, kOneSecondUs, false},
};

}  // namespace

std::optional<SensorTypeTraits> GetSensorTypeTraits(SensorType type) {
    for (const auto& traits : kTraits) {
        if (traits.type == type) {
            return traits;
        }
    }
    return std::nullopt;
}

int32_t GetReportingModeFlags(SensorType type) {
    auto traits = GetSensorTypeTraits(type);
    ReportingMode mode = traits ? traits->reporting_mode : ReportingMode::kContinuous;
    switch (mode) {
        case ReportingMode::kOnChange:
            return SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE;
        case ReportingMode::kOneShot:
            return SensorInfo::SENSOR_FLAG_BITS_ONE_SHOT_MODE;
        case ReportingMode::kSpecial:
            return SensorInfo::SENSOR_FLAG_BITS_SPECIAL_REPORTING_MODE;
        case ReportingMode::kContinuous:
        default:
            return SensorInfo::SENSOR_FLAG_BITS_CONTINUOUS_MODE;
    }
}

ReportingMode GetReportingMode(int32_t flags) {
    switch (flags & SensorInfo::SENSOR_FLAG_BITS_MASK_REPORTING_MODE) {
        case SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE:
            return ReportingMode::kOnChange;
        case SensorInfo::SENSOR_FLAG_BITS_ONE_SHOT_MODE:
            return ReportingMode::kOneShot;
        case SensorInfo::SENSOR_FLAG_BITS_SPECIAL_REPORTING_MODE:
            return ReportingMode::kSpecial;
        default:
            return ReportingMode::kContinuous;
    }
}

bool IsOnChangeSensor(int32_t flags) {
    return GetReportingMode(flags) == ReportingMode::kOnChange;
}

bool IsOneShotSensor(int32_t flags) {
    return GetReportingMode(flags) == ReportingMode::kOneShot;
}

bool IsWakeUpSensor(int32_t flags) {
    return (flags & SensorInfo::SENSOR_FLAG_BITS_WAKE_UP) != 0;
}

std::optional<SensorType> ParseSensorType(const std::string& name) {
    for (const auto& traits : kTraits) {
        if (::android::base::EqualsIgnoreCase(name, traits.config_name) ||
            ::android::base::EqualsIgnoreCase(name, toString(traits.type))) {
            return traits.type;
        }
    }
    // A few convenient aliases.
    if (::android::base::EqualsIgnoreCase(name, "accelerometer")) return SensorType::ACCELEROMETER;
    if (::android::base::EqualsIgnoreCase(name, "gyroscope")) return SensorType::GYROSCOPE;
    if (::android::base::EqualsIgnoreCase(name, "magnetometer") ||
        ::android::base::EqualsIgnoreCase(name, "compass")) {
        return SensorType::MAGNETIC_FIELD;
    }
    if (::android::base::EqualsIgnoreCase(name, "prox")) return SensorType::PROXIMITY;
    if (::android::base::EqualsIgnoreCase(name, "als")) return SensorType::LIGHT;
    if (::android::base::EqualsIgnoreCase(name, "baro") ||
        ::android::base::EqualsIgnoreCase(name, "barometer")) {
        return SensorType::PRESSURE;
    }
    if (::android::base::EqualsIgnoreCase(name, "temp")) return SensorType::AMBIENT_TEMPERATURE;
    return std::nullopt;
}

void ApplySensorTypeDefaults(SensorInfo* info) {
    auto traits = GetSensorTypeTraits(info->type);
    info->flags = GetReportingModeFlags(info->type);
    if (traits.has_value()) {
        info->maxRange = traits->default_max_range;
        info->resolution = traits->default_resolution;
        info->power = traits->default_power_ma;
        info->minDelayUs = traits->default_min_delay_us;
        info->maxDelayUs = traits->default_max_delay_us;
        if (traits->default_wake_up) {
            info->flags |= SensorInfo::SENSOR_FLAG_BITS_WAKE_UP;
        }
    } else {
        info->maxRange = 1.0f;
        info->resolution = 1.0f;
        info->power = 0.1f;
        info->minDelayUs = 10000;
        info->maxDelayUs = kOneSecondUs;
    }
    info->fifoReservedEventCount = 0;
    info->fifoMaxEventCount = 0;
    info->requiredPermission = "";
    info->typeAsString = "";
}

int64_t ClampSamplingPeriodNs(const SensorInfo& info, int64_t period_ns) {
    const int64_t min_ns = static_cast<int64_t>(info.minDelayUs) * 1000;
    const int64_t max_ns = static_cast<int64_t>(info.maxDelayUs) * 1000;
    if (min_ns > 0 && period_ns < min_ns) {
        period_ns = min_ns;
    }
    if (max_ns > 0 && period_ns > max_ns) {
        period_ns = max_ns;
    }
    return period_ns;
}

}  // namespace aidl::android::hardware::sensors::mainline
