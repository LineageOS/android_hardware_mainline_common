/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "CompositeSensor.h"

#include <aidl/android/hardware/sensors/BnSensors.h>

#include <libsensors_mainline/SensorBackend.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

class SensorBackendManager {
  public:
    SensorBackendManager();
    ~SensorBackendManager();

    SensorBackendManager(const SensorBackendManager&) = delete;
    SensorBackendManager& operator=(const SensorBackendManager&) = delete;

    void RegisterCompositeSensor(std::unique_ptr<ICompositeSensor> sensor);

    void LoadBackends();

    void Initialize(const PostEventsCallback& callback);

    void Deinitialize();

    std::vector<SensorInfo> GetSensorsList();

    int32_t Activate(int32_t sensor_handle, bool enabled);

    int32_t Batch(int32_t sensor_handle, int64_t sampling_period_ns, int64_t max_report_latency_ns);

    int32_t Flush(int32_t sensor_handle);

    int32_t SetOperationMode(OperationMode mode);

  private:
    struct BackendEntry {
        std::string name;
        void* dl_handle;
        std::unique_ptr<ISensorBackend> backend;
        bool initialized = false;
        std::vector<SensorInfo> sensors;
        std::map<int32_t, int32_t> local_to_global_handles;
        std::map<int32_t, int32_t> global_to_local_handles;
    };

    void LoadBackend(const std::string& library_name);

    int32_t GetBackendIndex(int32_t global_handle);

    std::vector<std::string> GetBackendList();

    std::vector<BackendEntry> backends_;
    int32_t next_handle_ = 1;
    std::map<int32_t, size_t> global_handle_to_backend_;
    std::mutex mutex_;
    std::mutex callback_mutex_;
    PostEventsCallback post_events_callback_;

    std::vector<std::unique_ptr<ICompositeSensor>> composite_sensors_;
    std::map<int32_t, size_t> composite_handle_to_index_;
    std::map<SensorType, std::vector<size_t>> sensor_type_to_composite_;
    std::map<int32_t, int32_t> hardware_dependency_count_;
    std::set<int32_t> directly_activated_;
    std::map<int32_t, int64_t> direct_sampling_period_ns_;
    std::map<size_t, int64_t> composite_sampling_period_ns_;
    std::mutex lifecycle_mutex_;

    std::vector<Event> ProcessCompositeSensors(const std::vector<Event>& events);
    int32_t FindHardwareSensorHandle(SensorType type);
    std::optional<int64_t> EffectiveSamplingPeriodLocked(int32_t hardware_handle);
};

}  // namespace aidl::android::hardware::sensors::mainline
