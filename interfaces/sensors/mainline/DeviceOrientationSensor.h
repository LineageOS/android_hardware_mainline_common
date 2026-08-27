/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "CompositeSensor.h"

#include <atomic>
#include <cstdint>

namespace aidl::android::hardware::sensors::mainline {

class DeviceOrientationSensor : public ICompositeSensor {
  public:
    DeviceOrientationSensor();
    ~DeviceOrientationSensor() override = default;

    CompositeSensorInfo GetSensorInfo() const override;

    void SetHandle(int32_t handle) override;

    std::vector<CompositeSensorType> GetInputSensorTypes() const override;

    std::vector<CompositeEvent> ProcessEvent(const CompositeEvent& input_event) override;

    void Activate(bool enabled) override;

    bool IsActive() const override;

    CompositeEvent CreateFlushCompleteEvent() const override;

  private:
    float ComputeOrientation(float x, float y, float z);

    CompositeSensorInfo sensor_info_;
    std::atomic_bool active_;
    float last_orientation_;
    bool has_last_orientation_;
};

}  // namespace aidl::android::hardware::sensors::mainline
