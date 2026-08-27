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
    static constexpr float kRadiansToDegrees = 180.0f / static_cast<float>(M_PI);
    static constexpr float kFilterTimeConstantMs = 200.0f;
    static constexpr int64_t kProposalSettleTimeNs = 40LL * 1000 * 1000;
    static constexpr int64_t kMaxFilterDeltaTimeNs = 1000LL * 1000 * 1000;
    static constexpr float kMaxTilt = 80.0f;
    static constexpr int32_t kAdjacentOrientationAngleGap = 45;
    static constexpr int32_t kTiltOverheadEnter = -40;
    static constexpr int32_t kTiltOverheadExit = -15;
    static constexpr float kNearZeroMagnitude = 1.0f;
    static constexpr float kAccelerationTolerance = 4.0f;
    static constexpr float kStandardGravity = 9.80665f;
    static constexpr float kMinAccelerationMagnitude = kStandardGravity - kAccelerationTolerance;
    static constexpr float kMaxAccelerationMagnitude = kStandardGravity + kAccelerationTolerance;

    static constexpr int32_t kTiltTolerance[4][2] = {
            {-25, 70},
            {-25, 65},
            {-25, 60},
            {-25, 65},
    };

    void LoadOrientationProperties();
    void TransformAxes(float& x, float& y, float& z) const;
    int32_t ApplyRotationOffset(int32_t orientation) const;
    void ResetFilterState();
    bool IsTiltAngleAcceptable(int32_t rotation, int32_t tilt_angle) const;
    bool IsOrientationAngleAcceptable(int32_t rotation, int32_t orientation_angle) const;

    CompositeSensorInfo sensor_info_;
    std::atomic_bool active_;
    int32_t last_orientation_;
    bool has_last_orientation_;

    bool swap_xy_;
    bool invert_x_;
    bool invert_y_;
    bool invert_z_;
    int32_t rotation_offset_;

    float filtered_x_;
    float filtered_y_;
    float filtered_z_;
    int64_t last_filter_timestamp_ns_;

    int32_t predicted_rotation_;
    int64_t predicted_rotation_timestamp_ns_;
    bool overhead_;
};

}  // namespace aidl::android::hardware::sensors::mainline
