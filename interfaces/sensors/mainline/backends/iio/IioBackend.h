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

struct IioChannelInfo {
    std::string name;
    int32_t index;
    char sign;
    uint8_t realbits;
    uint8_t storagebits;
    uint8_t shift;
    bool is_big_endian;
    float scale;
    float offset;
    int32_t location;
};

struct IioSensorData {
    int32_t handle;
    std::string sysfs_path;
    std::string device_name;
    SensorType type;
    SensorInfo sensor_info;
    std::vector<IioChannelInfo> channels;
    bool is_poll_mode;
    float mount_matrix[9];
    int32_t dev_num;
    std::atomic_bool enabled;
    int64_t sampling_period_ns;
    std::thread poll_thread;
    std::mutex poll_mutex;
    std::condition_variable poll_cv;
    std::atomic_bool stop_thread;
};

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
    void DiscoverDevices();
    void DiscoverSensors(int dev_num, const std::string& sysfs_path);
    bool ParseChannelType(const std::string& type_str, IioChannelInfo& channel);
    void ParseMountMatrix(const std::string& sysfs_path, float matrix[9]);
    bool ParseMountMatrixFromString(const std::string& content, float matrix[9]);
    float ReadSysfsFloat(const std::string& path, float default_value);
    int32_t ReadSysfsInt(const std::string& path, int32_t default_value);
    std::string ReadSysfsString(const std::string& path, const std::string& default_value);
    bool WriteSysfsInt(const std::string& path, int32_t value);

    void PollSensorThread(IioSensorData* sensor);
    std::vector<Event> ReadPollSensorData(IioSensorData* sensor);
    std::vector<Event> ReadBufferSensorData(IioSensorData* sensor);
    void EnableRingBuffer(IioSensorData* sensor, bool enable);

    int32_t MapIioTypeToSensorType(const std::string& iio_name);
    int32_t DetectTypeFromScanElements(const std::string& sysfs_path);
    int32_t DetectTypeFromSysfsAttributes(const std::string& sysfs_path);
    std::string ParseVendorFromCompatible(const std::string& of_compatible);
    bool IsVec3Type(SensorType type);

    void DeriveSensorInfoFromSysfs(IioSensorData* sensor);
    void ApplySensorInfoOverrides(IioSensorData* sensor);
    std::vector<float> ReadAvailableFrequencies(const std::string& sysfs_path);

    EventPayload::Vec3 BuildVec3Value(const std::vector<float>& values);

    std::map<int32_t, std::unique_ptr<IioSensorData>> sensors_;
    int32_t next_handle_ = 1;
    PostEventsCallback post_events_callback_;
    OperationMode operation_mode_ = OperationMode::NORMAL;
    std::mutex mutex_;
};

extern "C" ISensorBackend* CreateSensorBackend();

}  // namespace aidl::android::hardware::sensors::mainline
