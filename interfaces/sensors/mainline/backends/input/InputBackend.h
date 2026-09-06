/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_hwdb/SensorHwdb.h>
#include <libsensors_mainline/SensorBackend.h>

#include <memory>
#include <mutex>
#include <vector>

#include "InputDevice.h"

namespace aidl::android::hardware::sensors::mainline {

/*
 * Linux input subsystem backend.
 *
 * Enumerates /dev/input/event* and exposes accelerometers reported through
 * ABS_X/Y/Z (drivers/input/misc/bma150.c, mma8450.c, adxl34x.c, ...) and
 * proximity switches (SW_FRONT_PROXIMITY, usually gpio-keys).
 */
class InputBackend : public ISensorBackend {
  public:
    InputBackend();
    ~InputBackend() override;

    std::string GetName() const override;
    int32_t Initialize(const PostEventsCallback& callback) override;
    void Deinitialize() override;
    std::vector<SensorInfo> GetSensorsList() override;
    int32_t Activate(int32_t sensor_handle, bool enabled) override;
    int32_t Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                  int64_t max_report_latency_ns) override;
    int32_t Flush(int32_t sensor_handle) override;
    int32_t SetOperationMode(OperationMode mode) override;

  private:
    InputDevice* FindDevice(int32_t handle);

    std::mutex mutex_;
    std::unique_ptr<SensorHwdb> hwdb_;
    std::vector<std::unique_ptr<InputDevice>> devices_;
    int32_t next_handle_ = 1;
};

}  // namespace aidl::android::hardware::sensors::mainline
