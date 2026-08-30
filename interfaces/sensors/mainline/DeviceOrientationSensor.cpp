/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsComposite"

#include "DeviceOrientationSensor.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <cmath>
#include <cstdlib>

namespace aidl::android::hardware::sensors::mainline {

static constexpr int32_t kDefaultMaxDelayUs = 10 * 1000 * 1000;

static constexpr float kGravityThreshold = 7.0f;

static constexpr int32_t kRotation0 = 0;
static constexpr int32_t kRotation90 = 1;
static constexpr int32_t kRotation180 = 2;
static constexpr int32_t kRotation270 = 3;

static constexpr const char* kPropSwapXY = "vendor.sensors.orientation.swap_xy";
static constexpr const char* kPropInvertX = "vendor.sensors.orientation.invert_x";
static constexpr const char* kPropInvertY = "vendor.sensors.orientation.invert_y";
static constexpr const char* kPropInvertZ = "vendor.sensors.orientation.invert_z";
static constexpr const char* kPropRotationOffset = "vendor.sensors.orientation.rotation_offset";

DeviceOrientationSensor::DeviceOrientationSensor()
    : active_(false),
      last_orientation_(kRotation0),
      has_last_orientation_(false),
      predicted_rotation_(-1),
      predicted_rotation_time_(0),
      swap_xy_(false),
      invert_x_(false),
      invert_y_(false),
      invert_z_(false),
      rotation_offset_(0) {
    sensor_info_.sensorHandle = -1;
    sensor_info_.name = "Device Orientation";
    sensor_info_.vendor = "Mainline HAL";
    sensor_info_.version = 1;
    sensor_info_.type = CompositeSensorType::DEVICE_ORIENTATION;
    sensor_info_.typeAsString = "";
    sensor_info_.maxRange = 3.0f;
    sensor_info_.resolution = 1.0f;
    sensor_info_.power = 0.0f;
    sensor_info_.minDelayUs = 0;
    sensor_info_.maxDelayUs = kDefaultMaxDelayUs;
    sensor_info_.fifoReservedEventCount = 0;
    sensor_info_.fifoMaxEventCount = 0;
    sensor_info_.requiredPermission = "";
    sensor_info_.flags = static_cast<int32_t>(CompositeSensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
}

CompositeSensorInfo DeviceOrientationSensor::GetSensorInfo() const {
    return sensor_info_;
}

void DeviceOrientationSensor::SetHandle(int32_t handle) {
    sensor_info_.sensorHandle = handle;
}

std::vector<CompositeSensorType> DeviceOrientationSensor::GetInputSensorTypes() const {
    return {CompositeSensorType::ACCELEROMETER};
}

void DeviceOrientationSensor::LoadOrientationProperties() {
    swap_xy_ = ::android::base::GetBoolProperty(kPropSwapXY, false);
    invert_x_ = ::android::base::GetBoolProperty(kPropInvertX, false);
    invert_y_ = ::android::base::GetBoolProperty(kPropInvertY, false);
    invert_z_ = ::android::base::GetBoolProperty(kPropInvertZ, false);

    std::string offset_str = ::android::base::GetProperty(kPropRotationOffset, "0");
    int32_t offset_deg = 0;
    char* end = nullptr;
    long val = std::strtol(offset_str.c_str(), &end, 10);
    if (end != offset_str.c_str() && *end == '\0') {
        offset_deg = static_cast<int32_t>(val);
    }

    if (offset_deg == 90 || offset_deg == 180 || offset_deg == 270) {
        rotation_offset_ = offset_deg / 90;
    } else {
        rotation_offset_ = 0;
    }

    float matrix[3][3] = {
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
    };

    if (invert_x_) {
        matrix[0][0] *= -1.0f;
        matrix[1][0] *= -1.0f;
        matrix[2][0] *= -1.0f;
    }
    if (invert_y_) {
        matrix[0][1] *= -1.0f;
        matrix[1][1] *= -1.0f;
        matrix[2][1] *= -1.0f;
    }
    if (invert_z_) {
        matrix[0][2] *= -1.0f;
        matrix[1][2] *= -1.0f;
        matrix[2][2] *= -1.0f;
    }
    if (swap_xy_) {
        std::swap(matrix[0][0], matrix[0][1]);
        std::swap(matrix[1][0], matrix[1][1]);
        std::swap(matrix[2][0], matrix[2][1]);
    }

    LOG(INFO) << "DeviceOrientationSensor workaround config: "
              << "swap_xy=" << swap_xy_ << " invert_x=" << invert_x_ << " invert_y=" << invert_y_
              << " invert_z=" << invert_z_ << " rotation_offset=" << (rotation_offset_ * 90) << "°";
    LOG(INFO) << "DeviceOrientationSensor effective mount matrix: "
              << "[" << matrix[0][0] << "," << matrix[0][1] << "," << matrix[0][2] << "; "
              << matrix[1][0] << "," << matrix[1][1] << "," << matrix[1][2] << "; " << matrix[2][0]
              << "," << matrix[2][1] << "," << matrix[2][2] << "]";
}

void DeviceOrientationSensor::TransformAxes(float& x, float& y, float& z) const {
    if (invert_x_) x = -x;
    if (invert_y_) y = -y;
    if (invert_z_) z = -z;
    if (swap_xy_) std::swap(x, y);
}

int32_t DeviceOrientationSensor::ApplyRotationOffset(int32_t orientation) const {
    if (rotation_offset_ == 0) {
        return orientation;
    }
    return (orientation + rotation_offset_) % 4;
}

void DeviceOrientationSensor::Activate(bool enabled) {
    active_ = enabled;
    if (enabled) {
        has_last_orientation_ = false;
        predicted_rotation_ = -1;
        predicted_rotation_time_ = 0;
        LoadOrientationProperties();
    } else {
        has_last_orientation_ = false;
        predicted_rotation_ = -1;
        predicted_rotation_time_ = 0;
    }
    LOG(DEBUG) << "DeviceOrientationSensor " << (enabled ? "activated" : "deactivated");
}

bool DeviceOrientationSensor::IsActive() const {
    return active_.load();
}

CompositeEvent DeviceOrientationSensor::CreateFlushCompleteEvent() const {
    CompositeEvent ev;
    ev.sensorHandle = sensor_info_.sensorHandle;
    ev.sensorType = CompositeSensorType::META_DATA;
    CompositeEventPayload::MetaData meta = {
            .what = CompositeEventPayload::MetaData::MetaDataEventType::META_DATA_FLUSH_COMPLETE,
    };
    ev.payload.set<CompositeEventPayload::Tag::meta>(meta);
    return ev;
}

int32_t DeviceOrientationSensor::ComputeOrientation(float x, float y, float z) {
    float gravity_magnitude = std::sqrt(x * x + y * y + z * z);
    if (gravity_magnitude < kGravityThreshold) {
        return has_last_orientation_ ? last_orientation_ : kRotation0;
    }

    float abs_x = std::fabs(x);
    float abs_y = std::fabs(y);

    if (y > abs_x) {
        return kRotation0;
    } else if (x > abs_y) {
        return kRotation90;
    } else if (y < -abs_x) {
        return kRotation180;
    } else if (x < -abs_y) {
        return kRotation270;
    }

    if (has_last_orientation_) {
        return last_orientation_;
    }

    return kRotation0;
}

std::vector<CompositeEvent> DeviceOrientationSensor::ProcessEvent(
        const CompositeEvent& input_event) {
    std::vector<CompositeEvent> output;

    if (!active_.load()) {
        return output;
    }

    if (input_event.sensorType != CompositeSensorType::ACCELEROMETER) {
        return output;
    }

    if (input_event.payload.getTag() != CompositeEventPayload::Tag::vec3) {
        return output;
    }

    const auto& vec3 = input_event.payload.get<CompositeEventPayload::Tag::vec3>();

    float x = vec3.x;
    float y = vec3.y;
    float z = vec3.z;
    TransformAxes(x, y, z);

    int64_t now = input_event.timestamp;

    int32_t predicted = ComputeOrientation(x, y, z);
    predicted = ApplyRotationOffset(predicted);

    if (predicted != predicted_rotation_) {
        predicted_rotation_ = predicted;
        predicted_rotation_time_ = now;
        return output;
    }

    if (now < predicted_rotation_time_ + kSettleTimeNs) {
        return output;
    }

    if (!has_last_orientation_ || predicted != last_orientation_) {
        CompositeEvent event;
        event.sensorHandle = sensor_info_.sensorHandle;
        event.sensorType = CompositeSensorType::DEVICE_ORIENTATION;
        event.timestamp = now;
        event.payload.set<CompositeEventPayload::Tag::scalar>(static_cast<float>(predicted));

        output.push_back(event);

        last_orientation_ = predicted;
        has_last_orientation_ = true;

        LOG(DEBUG) << "DeviceOrientation changed to rotation=" << predicted;
    }

    return output;
}

}  // namespace aidl::android::hardware::sensors::mainline
