/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_common/MountMatrix.h>

#include <cstdint>

#include "CompositeSensor.h"

namespace aidl::android::hardware::sensors::mainline {

/*
 * DEVICE_ORIENTATION sensor computed from the accelerometer.
 *
 * Reports 0..3 (see SensorType::DEVICE_ORIENTATION) with angular hysteresis
 * and a settle time so that transient movements do not flip the orientation.
 *
 * Disabled by default (the AIDL documentation asks for hardware
 * implementations); enable it with the setting
 * "composite.device_orientation.enabled".
 *
 * Workarounds for devices whose accelerometer mount matrix is wrong can be
 * applied through the "orientation.*" settings, which are re-read on every
 * activation:
 *   orientation.swap_xy, orientation.invert_x, orientation.invert_y,
 *   orientation.invert_z: axis transformation applied to the accelerometer
 *   orientation.rotation_offset: 0/90/180/270 degrees added to the result
 */
class DeviceOrientationSensor : public CompositeSensorBase {
  public:
    DeviceOrientationSensor();

    std::vector<SensorType> GetInputSensorTypes() const override;
    int64_t GetInputSamplingPeriodNs() const override;
    void Activate(bool enabled) override;
    std::vector<Event> ProcessEvent(const Event& input_event) override;

  private:
    void LoadWorkarounds();
    int32_t ComputeRotation(float x, float y, float z) const;

    MountMatrix workaround_matrix_;
    int32_t rotation_offset_ = 0;

    int32_t reported_rotation_ = -1;
    int32_t candidate_rotation_ = -1;
    int64_t candidate_since_ns_ = 0;
};

}  // namespace aidl::android::hardware::sensors::mainline
