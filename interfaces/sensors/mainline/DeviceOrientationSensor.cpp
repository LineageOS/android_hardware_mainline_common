/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsComposite"

#include "DeviceOrientationSensor.h"

#include <android-base/logging.h>

#include <cmath>

namespace aidl::android::hardware::sensors::mainline {

static constexpr int32_t kDefaultMaxDelayUs = 10 * 1000 * 1000;

static constexpr float kGravityThreshold = 7.0f;

static constexpr int32_t kRotation0 = 0;
static constexpr int32_t kRotation90 = 1;
static constexpr int32_t kRotation180 = 2;
static constexpr int32_t kRotation270 = 3;

DeviceOrientationSensor::DeviceOrientationSensor()
    : active_(false), last_orientation_(kRotation0), has_last_orientation_(false) {
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
    sensor_info_.flags =
            static_cast<int32_t>(CompositeSensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
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

void DeviceOrientationSensor::Activate(bool enabled) {
    active_ = enabled;
    if (!enabled) {
        has_last_orientation_ = false;
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

    int32_t orientation = ComputeOrientation(vec3.x, vec3.y, vec3.z);

    if (!has_last_orientation_ || orientation != last_orientation_) {
        CompositeEvent event;
        event.sensorHandle = sensor_info_.sensorHandle;
        event.sensorType = CompositeSensorType::DEVICE_ORIENTATION;
        event.timestamp = input_event.timestamp;
        event.payload.set<CompositeEventPayload::Tag::scalar>(static_cast<float>(orientation));

        output.push_back(event);

        last_orientation_ = orientation;
        has_last_orientation_ = true;

        LOG(DEBUG) << "DeviceOrientation changed to rotation=" << orientation;
    }

    return output;
}

}  // namespace aidl::android::hardware::sensors::mainline
