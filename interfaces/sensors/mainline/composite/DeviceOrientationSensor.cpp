/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsComposite"

#include "DeviceOrientationSensor.h"

#include <android-base/logging.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/SensorTypes.h>
#include <libsensors_common/Settings.h>

#include <cmath>
#include <string>
#include <utility>

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr int64_t kInputPeriodNs = 66LL * 1000 * 1000;  // ~15 Hz
constexpr int64_t kSettleTimeNs = 250LL * 1000 * 1000;
// The device is considered flat (no orientation change) when the gravity
// component in the screen plane is below this fraction of g.
constexpr float kFlatThreshold = 0.35f * 9.80665f;
// A new orientation is only accepted when the gravity vector is within this
// angle of the candidate's centre (45 degrees would be the bare boundary).
constexpr float kAcceptAngleDeg = 30.0f;

std::string SettingKey(const std::string& name) {
    return std::string(DeviceOrientationSensor::kSettingPrefix) + "." + name;
}

}  // namespace

DeviceOrientationSensor::DeviceOrientationSensor() {
    info_.sensorHandle = -1;
    info_.type = SensorType::DEVICE_ORIENTATION;
    ApplySensorTypeDefaults(&info_);
    info_.name = "Device Orientation";
    info_.vendor = "Mainline Sensors HAL";
    info_.version = 1;
    info_.maxRange = 3.0f;
    info_.resolution = 1.0f;
    info_.power = 0.0f;
}

std::vector<SensorType> DeviceOrientationSensor::GetInputSensorTypes() const {
    return {SensorType::ACCELEROMETER};
}

int64_t DeviceOrientationSensor::GetInputSamplingPeriodNs() const {
    return kInputPeriodNs;
}

void DeviceOrientationSensor::LoadWorkarounds() {
    Settings& settings = Settings::Get();
    const bool swap_xy = settings.GetBool(SettingKey("swap_xy"), false);
    const bool invert_x = settings.GetBool(SettingKey("invert_x"), false);
    const bool invert_y = settings.GetBool(SettingKey("invert_y"), false);
    const bool invert_z = settings.GetBool(SettingKey("invert_z"), false);
    const int64_t offset_deg = settings.GetInt(SettingKey("rotation_offset"), 0);

    rotation_offset_ = 0;
    if (offset_deg == 90 || offset_deg == 180 || offset_deg == 270) {
        rotation_offset_ = static_cast<int32_t>(offset_deg / 90);
    } else if (offset_deg != 0) {
        LOG(WARNING) << SettingKey("rotation_offset") << " must be 0, 90, 180 or 270; got "
                     << offset_deg;
    }

    // Build the transformation as a matrix: out = M * in.
    workaround_matrix_ = MountMatrix::Identity();
    if (invert_x) workaround_matrix_.at(0, 0) = -1.0f;
    if (invert_y) workaround_matrix_.at(1, 1) = -1.0f;
    if (invert_z) workaround_matrix_.at(2, 2) = -1.0f;
    if (swap_xy) {
        for (int col = 0; col < 3; col++) {
            std::swap(workaround_matrix_.at(0, col), workaround_matrix_.at(1, col));
        }
    }
    LOG(INFO) << "Device orientation workarounds: swap_xy=" << swap_xy << " invert_x=" << invert_x
              << " invert_y=" << invert_y << " invert_z=" << invert_z
              << " rotation_offset=" << rotation_offset_ * 90 << " deg, matrix ["
              << workaround_matrix_.ToString() << "]";
}

void DeviceOrientationSensor::Activate(bool enabled) {
    active_ = enabled;
    reported_rotation_ = -1;
    candidate_rotation_ = -1;
    candidate_since_ns_ = 0;
    if (enabled) {
        LoadWorkarounds();
    }
    LOG(INFO) << "Device orientation sensor " << (enabled ? "activated" : "deactivated");
}

int32_t DeviceOrientationSensor::ComputeRotation(float x, float y, float z) const {
    (void)z;
    const float plane = std::sqrt(x * x + y * y);
    if (plane < kFlatThreshold) {
        return -1;  // Flat: keep the current orientation.
    }
    // Angle of the gravity vector in the screen plane: 0 = +Y (portrait),
    // 90 = +X (rotated 90 degrees counter-clockwise), ...
    float angle = std::atan2(x, y) * 180.0f / static_cast<float>(M_PI);
    if (angle < 0.0f) {
        angle += 360.0f;
    }
    const int32_t candidate = static_cast<int32_t>(std::lround(angle / 90.0f)) % 4;
    float distance = std::fabs(angle - static_cast<float>(candidate) * 90.0f);
    if (distance > 180.0f) {
        distance = 360.0f - distance;
    }
    if (distance > kAcceptAngleDeg) {
        return -1;  // Too close to the boundary between two orientations.
    }
    return (candidate + rotation_offset_) % 4;
}

std::vector<Event> DeviceOrientationSensor::ProcessEvent(const Event& input_event) {
    std::vector<Event> output;
    if (!active_ || input_event.sensorType != SensorType::ACCELEROMETER ||
        input_event.payload.getTag() != EventPayload::Tag::vec3) {
        return output;
    }

    const auto& vec3 = input_event.payload.get<EventPayload::Tag::vec3>();
    float x = vec3.x;
    float y = vec3.y;
    float z = vec3.z;
    workaround_matrix_.Apply(&x, &y, &z);

    const int32_t rotation = ComputeRotation(x, y, z);
    const int64_t now = input_event.timestamp;
    if (rotation < 0) {
        candidate_rotation_ = -1;
        return output;
    }
    if (rotation != candidate_rotation_) {
        candidate_rotation_ = rotation;
        candidate_since_ns_ = now;
    }
    if (rotation == reported_rotation_) {
        return output;
    }
    // Report immediately on activation, otherwise wait for the settle time.
    if (reported_rotation_ >= 0 && now - candidate_since_ns_ < kSettleTimeNs) {
        return output;
    }

    reported_rotation_ = rotation;
    output.push_back(
            MakeScalarEvent(info_.sensorHandle, info_.type, now, static_cast<float>(rotation)));
    LOG(DEBUG) << "Device orientation -> " << rotation;
    return output;
}

}  // namespace aidl::android::hardware::sensors::mainline
