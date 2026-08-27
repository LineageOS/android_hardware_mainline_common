/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_mainline/SensorBackend.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

struct InputSensorData {
    int32_t handle;
    std::string device_path;
    std::string device_name;
    SensorType type;
    SensorInfo sensor_info;
    bool is_switch;
    std::atomic_bool enabled;
    int64_t sampling_period_ns;
    std::thread poll_thread;
    std::mutex poll_mutex;
    std::condition_variable poll_cv;
    std::atomic_bool stop_thread;
    float last_value;
};

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
    void DiscoverDevices();
    bool CheckInputDeviceHasSensor(const std::string& sysfs_path, int& sensor_type_out,
                                   bool& is_switch_out);
    bool HasSwitchCapability(const std::string& sysfs_path);
    bool HasAbsoluteAxes(const std::string& sysfs_path);

    void PollSensorThread(InputSensorData* sensor);
    std::vector<Event> ReadSensorData(InputSensorData* sensor);

    std::string ReadSysfsString(const std::string& path, const std::string& default_value);

    std::map<int32_t, std::unique_ptr<InputSensorData>> sensors_;
    int32_t next_handle_ = 1;
    PostEventsCallback post_events_callback_;
    OperationMode operation_mode_ = OperationMode::NORMAL;
    std::mutex mutex_;
};

extern "C" ISensorBackend* CreateSensorBackend();

}  // namespace aidl::android::hardware::sensors::mainline
