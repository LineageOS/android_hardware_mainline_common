/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_mainline/SensorBackend.h>

#include <cstdint>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

/*
 * A composite (virtual) sensor derives its data from the events of one or
 * more hardware sensors provided by the backends. Composite sensors live in
 * the frontend and are managed by SensorManager, which:
 *  - only registers a composite sensor if no backend provides its type,
 *  - activates the hardware sensors it depends on while it is active,
 *  - feeds it the events of those hardware sensors (regardless of whether the
 *    framework itself subscribed to them).
 *
 * All methods are called with the SensorManager lock held.
 */
class ICompositeSensor {
  public:
    virtual ~ICompositeSensor() = default;

    // Sensor description. The handle is assigned by the manager via SetHandle()
    // before the sensor is used.
    virtual const SensorInfo& GetSensorInfo() const = 0;
    virtual void SetHandle(int32_t handle) = 0;

    // Hardware sensor types this sensor needs as input.
    virtual std::vector<SensorType> GetInputSensorTypes() const = 0;

    // Sampling period to request from the input sensors.
    virtual int64_t GetInputSamplingPeriodNs() const = 0;

    virtual void Activate(bool enabled) = 0;
    virtual bool IsActive() const = 0;

    // Requested output period; may be ignored by on-change sensors.
    virtual void Batch(int64_t sampling_period_ns) = 0;

    // Processes one input event and returns the events to deliver (possibly
    // none). The handle of returned events must be the composite handle.
    virtual std::vector<Event> ProcessEvent(const Event& input_event) = 0;
};

// Convenience base class holding the common state.
class CompositeSensorBase : public ICompositeSensor {
  public:
    const SensorInfo& GetSensorInfo() const override { return info_; }
    void SetHandle(int32_t handle) override { info_.sensorHandle = handle; }
    bool IsActive() const override { return active_; }
    void Batch(int64_t sampling_period_ns) override { period_ns_ = sampling_period_ns; }

  protected:
    SensorInfo info_;
    bool active_ = false;
    int64_t period_ns_ = 0;
};

}  // namespace aidl::android::hardware::sensors::mainline
