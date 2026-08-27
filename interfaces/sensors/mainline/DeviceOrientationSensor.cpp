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

DeviceOrientationSensor::DeviceOrientationSensor()
    : active_(false), last_orientation_(0.0f), has_last_orientation_(false) {
    sensor_info_.sensorHandle = -1;
    sensor_info_.name = "Device Orientation";
    sensor_info_.vendor = "Mainline HAL";
    sensor_info_.version = 1;
    sensor_info_.type = CompositeSensorType::DEVICE_ORIENTATION;
    sensor_info_.typeAsString = "";
    sensor_info_.maxRange = 360.0f;
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

float DeviceOrientationSensor::ComputeOrientation(float x, float y, float z) {
    float angle = std::atan2(-x, y) * 180.0f / static_cast<float>(M_PI);

    constexpr float kHysteresis = 10.0f;

    if (!has_last_orientation_) {
        if (angle >= -45.0f && angle < 45.0f) {
            return 0.0f;
        } else if (angle >= 45.0f && angle < 135.0f) {
            return 90.0f;
        } else if (angle >= -135.0f && angle < -45.0f) {
            return 270.0f;
        } else {
            return 180.0f;
        }
    }

    float current = last_orientation_;

    if (current == 0.0f) {
        if (angle > 45.0f + kHysteresis && angle < 135.0f - kHysteresis) {
            return 90.0f;
        } else if (angle < -45.0f - kHysteresis && angle > -135.0f + kHysteresis) {
            return 270.0f;
        } else if (angle > 135.0f + kHysteresis || angle < -135.0f - kHysteresis) {
            return 180.0f;
        }
    } else if (current == 90.0f) {
        if (angle < 45.0f - kHysteresis && angle > -45.0f + kHysteresis) {
            return 0.0f;
        } else if (angle > 135.0f + kHysteresis) {
            return 180.0f;
        } else if (angle < -135.0f + kHysteresis) {
            return 180.0f;
        }
    } else if (current == 180.0f) {
        if (angle > 45.0f + kHysteresis && angle < 135.0f - kHysteresis) {
            return 90.0f;
        } else if (angle < -45.0f - kHysteresis && angle > -135.0f + kHysteresis) {
            return 270.0f;
        } else if (angle > -135.0f + kHysteresis && angle < 135.0f - kHysteresis) {
            return 0.0f;
        }
    } else if (current == 270.0f) {
        if (angle < 45.0f - kHysteresis && angle > -45.0f + kHysteresis) {
            return 0.0f;
        } else if (angle > 135.0f + kHysteresis) {
            return 180.0f;
        } else if (angle > 45.0f + kHysteresis && angle < 135.0f - kHysteresis) {
            return 90.0f;
        }
    }

    return current;
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

    float orientation = ComputeOrientation(vec3.x, vec3.y, vec3.z);

    if (!has_last_orientation_ || orientation != last_orientation_) {
        CompositeEvent event;
        event.sensorHandle = sensor_info_.sensorHandle;
        event.sensorType = CompositeSensorType::DEVICE_ORIENTATION;
        event.timestamp = input_event.timestamp;
        event.payload.set<CompositeEventPayload::Tag::scalar>(orientation);

        output.push_back(event);

        last_orientation_ = orientation;
        has_last_orientation_ = true;

        LOG(DEBUG) << "DeviceOrientation changed to " << orientation;
    }

    return output;
}

}  // namespace aidl::android::hardware::sensors::mainline
