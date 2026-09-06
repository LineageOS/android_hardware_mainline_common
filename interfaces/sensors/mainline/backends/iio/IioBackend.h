/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_hwdb/SensorHwdb.h>
#include <libsensors_mainline/SensorBackend.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "IioDevice.h"

namespace aidl::android::hardware::sensors::mainline {

/*
 * Linux IIO subsystem backend.
 *
 * Enumerates /sys/bus/iio/devices/iio:device*, classifies their channels into
 * Android sensors and serves them through the kernel ring buffer or by polling
 * sysfs. See README.md in this directory for the details.
 */
class IioBackend : public ISensorBackend {
  public:
    IioBackend();
    ~IioBackend() override;

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
    void WaitForDevices();
    void DiscoverDevices();
    void MakeSensorNamesUnique();
    IioDevice* FindDevice(int32_t handle);

    std::mutex mutex_;
    std::unique_ptr<SensorHwdb> hwdb_;
    std::vector<std::unique_ptr<IioDevice>> devices_;
    std::map<int32_t, IioDevice*> handle_to_device_;
    int32_t next_handle_ = 1;
};

}  // namespace aidl::android::hardware::sensors::mainline
