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
    static constexpr int64_t kSettleTimeNs = 200 * 1000 * 1000;

    int32_t ComputeOrientation(float x, float y, float z);
    void LoadOrientationProperties();
    void TransformAxes(float& x, float& y, float& z) const;
    int32_t ApplyRotationOffset(int32_t orientation) const;

    CompositeSensorInfo sensor_info_;
    std::atomic_bool active_;
    int32_t last_orientation_;
    bool has_last_orientation_;

    int32_t predicted_rotation_;
    int64_t predicted_rotation_time_;

    bool swap_xy_;
    bool invert_x_;
    bool invert_y_;
    bool invert_z_;
    int32_t rotation_offset_;
};

}  // namespace aidl::android::hardware::sensors::mainline
