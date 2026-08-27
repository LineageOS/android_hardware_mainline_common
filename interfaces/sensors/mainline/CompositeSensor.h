/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <aidl/android/hardware/sensors/SensorType.h>

#include <vector>

namespace aidl::android::hardware::sensors::mainline {

using CompositeEvent = ::aidl::android::hardware::sensors::Event;
using CompositeEventPayload = ::aidl::android::hardware::sensors::Event::EventPayload;
using CompositeSensorInfo = ::aidl::android::hardware::sensors::SensorInfo;
using CompositeSensorType = ::aidl::android::hardware::sensors::SensorType;

class ICompositeSensor {
  public:
    virtual ~ICompositeSensor() = default;

    virtual CompositeSensorInfo GetSensorInfo() const = 0;

    virtual void SetHandle(int32_t handle) = 0;

    virtual std::vector<CompositeSensorType> GetInputSensorTypes() const = 0;

    virtual std::vector<CompositeEvent> ProcessEvent(const CompositeEvent& input_event) = 0;

    virtual void Activate(bool enabled) = 0;

    virtual bool IsActive() const = 0;

    virtual CompositeEvent CreateFlushCompleteEvent() const = 0;
};

}  // namespace aidl::android::hardware::sensors::mainline
