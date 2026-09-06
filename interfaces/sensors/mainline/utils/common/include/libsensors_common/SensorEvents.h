/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <aidl/android/hardware/sensors/SensorStatus.h>
#include <aidl/android/hardware/sensors/SensorType.h>

#include <cstdint>
#include <string>

namespace aidl::android::hardware::sensors::mainline {

// Current CLOCK_BOOTTIME in nanoseconds; the time base of Event::timestamp.
int64_t GetBootTimeNs();

// Builds an event with the given handle, type and timestamp; the payload is
// left to the caller.
::aidl::android::hardware::sensors::Event MakeEvent(
        int32_t handle, ::aidl::android::hardware::sensors::SensorType type, int64_t timestamp_ns);

// Event with a Vec3 payload (accelerometer, gyroscope, magnetometer, ...).
::aidl::android::hardware::sensors::Event MakeVec3Event(
        int32_t handle, ::aidl::android::hardware::sensors::SensorType type, int64_t timestamp_ns,
        float x, float y, float z,
        ::aidl::android::hardware::sensors::SensorStatus status =
                ::aidl::android::hardware::sensors::SensorStatus::ACCURACY_HIGH);

// Event with a scalar payload (light, proximity, pressure, ...).
::aidl::android::hardware::sensors::Event MakeScalarEvent(
        int32_t handle, ::aidl::android::hardware::sensors::SensorType type, int64_t timestamp_ns,
        float value);

// Event with a step count payload.
::aidl::android::hardware::sensors::Event MakeStepCountEvent(int32_t handle, int64_t timestamp_ns,
                                                             int64_t steps);

// Event with a Vec4 payload (GAME_ROTATION_VECTOR).
::aidl::android::hardware::sensors::Event MakeVec4Event(
        int32_t handle, ::aidl::android::hardware::sensors::SensorType type, int64_t timestamp_ns,
        float x, float y, float z, float w);

// Event with a Data payload holding a quaternion and an estimated accuracy
// (ROTATION_VECTOR, GEOMAGNETIC_ROTATION_VECTOR).
::aidl::android::hardware::sensors::Event MakeRotationVectorEvent(
        int32_t handle, ::aidl::android::hardware::sensors::SensorType type, int64_t timestamp_ns,
        float x, float y, float z, float w, float accuracy_rad);

// META_DATA / FLUSH_COMPLETE event for the given sensor.
::aidl::android::hardware::sensors::Event MakeFlushCompleteEvent(int32_t handle);

// Returns true if the event is a META_DATA FLUSH_COMPLETE event.
bool IsFlushCompleteEvent(const ::aidl::android::hardware::sensors::Event& event);

// Returns true if both events carry the same payload (used by on-change
// sensors to suppress duplicates).
bool HaveSamePayload(const ::aidl::android::hardware::sensors::Event& a,
                     const ::aidl::android::hardware::sensors::Event& b);

// Short human readable description of an event for debug logs.
std::string EventToString(const ::aidl::android::hardware::sensors::Event& event);

// Short human readable description of a sensor for logs, e.g.
// "handle=3 type=ACCELEROMETER name='bmi160 Accelerometer' vendor='Bosch'".
std::string SensorInfoToString(const ::aidl::android::hardware::sensors::SensorInfo& info);

}  // namespace aidl::android::hardware::sensors::mainline
