/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <aidl/android/hardware/sensors/SensorType.h>

#include <cstdint>
#include <optional>
#include <string>

namespace aidl::android::hardware::sensors::mainline {

// Reporting mode of a sensor type, as defined by the Android sensor stack.
enum class ReportingMode {
    kContinuous,
    kOnChange,
    kOneShot,
    kSpecial,
};

// Static knowledge about an Android sensor type.
struct SensorTypeTraits {
    ::aidl::android::hardware::sensors::SensorType type;
    // Human readable label used to build sensor names, e.g. "Accelerometer".
    const char* label;
    // Short identifier used in configuration keys, e.g. "accel".
    const char* config_name;
    ReportingMode reporting_mode;
    // True for types whose payload is Vec3 (x, y, z).
    bool is_vec3;
    // Sensible defaults when the hardware does not tell better.
    float default_max_range;
    float default_resolution;
    float default_power_ma;
    int32_t default_min_delay_us;
    int32_t default_max_delay_us;
    // Whether the type is usually exposed as a wake-up sensor on Android.
    bool default_wake_up;
};

// Returns the traits of a known type, or nullopt for unknown/private types.
std::optional<SensorTypeTraits> GetSensorTypeTraits(
        ::aidl::android::hardware::sensors::SensorType type);

// Returns the SENSOR_FLAG_BITS_* reporting mode bits for a type.
int32_t GetReportingModeFlags(::aidl::android::hardware::sensors::SensorType type);

// Extracts the reporting mode from SensorInfo::flags.
ReportingMode GetReportingMode(int32_t flags);

bool IsOnChangeSensor(int32_t flags);
bool IsOneShotSensor(int32_t flags);
bool IsWakeUpSensor(int32_t flags);

// Parses a type from its configuration name (e.g. "accel", "light") or its AIDL
// enum name (e.g. "ACCELEROMETER"). Case insensitive.
std::optional<::aidl::android::hardware::sensors::SensorType> ParseSensorType(
        const std::string& name);

// Fills the type dependent defaults of a SensorInfo (flags, ranges, delays,
// power). Name, vendor and handle are left untouched.
void ApplySensorTypeDefaults(::aidl::android::hardware::sensors::SensorInfo* info);

// Clamps a requested sampling period to [minDelayUs, maxDelayUs] of the sensor
// (on-change/one-shot sensors with minDelayUs <= 0 keep the request as is,
// except for the upper bound).
int64_t ClampSamplingPeriodNs(const ::aidl::android::hardware::sensors::SensorInfo& info,
                              int64_t period_ns);

}  // namespace aidl::android::hardware::sensors::mainline
