/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "IioScan.h"

#include <SensorHwdb.h>
#include <android-base/unique_fd.h>
#include <libsensors_mainline/SensorBackend.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

struct IioChannelInfo {
    std::string name;
    int index = -1;
    iio::ScanType scan_type;
    double scale = 1.0;
    double offset = 0.0;
    size_t location = 0;
};

struct IioDirectSource {
    std::string path;
    bool is_input = false;
    double scale = 1.0;
    double offset = 0.0;
};

struct IioSensorData;

struct IioDeviceState {
    int dev_num = -1;
    std::string sysfs_path;
    std::string device_name;
    std::vector<IioChannelInfo> available_channels;
    std::vector<IioChannelInfo> effective_channels;
    size_t scan_size = 0;

    ::android::base::unique_fd buffer_fd;
    int signal_pipe_fd[2] = {-1, -1};
    std::thread reader_thread;
    std::atomic_bool reader_running{false};
    std::atomic_bool stop_reader{false};

    std::mutex config_mutex;
    std::mutex mutex;
    std::vector<IioSensorData*> sensors;
    std::vector<int32_t> pending_flushes;
    std::vector<uint8_t> partial_data;
    int64_t last_timestamp_ns = 0;
    int64_t last_receive_timestamp_ns = 0;
    std::atomic<int64_t> physical_period_ns{200000000};

    std::string trigger_name;
    bool trigger_attached_by_hal = false;
    bool hrtimer_trigger = false;
    bool hrtimer_owned = false;
};

struct IioSensorData {
    int32_t handle = 0;
    int dev_num = -1;
    std::string sysfs_path;
    std::string device_name;
    SensorType type = SensorType::META_DATA;
    SensorInfo sensor_info;
    std::vector<IioChannelInfo> channels;
    std::vector<IioDirectSource> direct_sources;
    std::shared_ptr<IioDeviceState> device;
    float mount_matrix[9] = {};
    std::string parent_modalias;
    std::string label;
    int proximity_near_level = -1;

    std::atomic_bool enabled{false};
    std::atomic_bool direct_poll{false};
    std::atomic_bool stop_thread{false};
    std::atomic<int64_t> sampling_period_ns{200000000};
    std::thread poll_thread;
    std::mutex lifecycle_mutex;
    std::mutex poll_mutex;
    std::condition_variable poll_cv;
    std::mutex event_mutex;
    int64_t last_delivered_timestamp_ns = 0;
    std::optional<std::vector<float>> last_value;
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
    void DiscoverDevice(int dev_num, const std::string& path);
    std::vector<IioChannelInfo> ReadScanChannels(const std::string& path);
    std::set<SensorType> DetectTypes(const std::string& path, const std::string& name,
                                     const std::string& compatible, const std::string& of_name,
                                     const std::vector<IioChannelInfo>& channels);
    std::optional<SensorType> MapIioType(const std::string& name) const;
    std::optional<SensorType> ClassifyChannel(const std::string& name) const;
    std::vector<IioDirectSource> FindDirectSources(const std::string& path, SensorType type);
    void InitializeSensorInfo(IioSensorData* sensor, const std::string& compatible);
    void DeriveSensorInfo(IioSensorData* sensor);
    void ApplyHwdb(IioSensorData* sensor);
    void ApplyOverrides(IioSensorData* sensor);
    void ParseMountMatrix(const std::string& path, SensorType type, float matrix[9]);
    bool ParseMountMatrixString(const std::string& text, float matrix[9]);

    bool ReconfigureDevice(const std::shared_ptr<IioDeviceState>& device);
    bool ConfigureBuffer(IioDeviceState* device, const std::vector<IioSensorData*>& active);
    bool ConfigureScanMask(IioDeviceState* device, const std::vector<IioSensorData*>& active);
    bool TryEnableBuffer(IioDeviceState* device);
    bool UseExistingOrDeviceTrigger(IioDeviceState* device);
    bool CreateHrtimerTrigger(IioDeviceState* device);
    void ReleaseOwnedTrigger(IioDeviceState* device);
    void DisableBuffer(IioDeviceState* device);
    void StopReader(IioDeviceState* device);
    void StartReader(const std::shared_ptr<IioDeviceState>& device);
    void ReaderThread(std::shared_ptr<IioDeviceState> device);
    void PollThread(IioSensorData* sensor);

    std::vector<Event> ParseScans(IioSensorData* sensor, const uint8_t* data, size_t scans,
                                  const std::vector<int64_t>& timestamps);
    std::optional<Event> ReadDirectEvent(IioSensorData* sensor);
    std::optional<Event> BuildEvent(IioSensorData* sensor, std::vector<float> values,
                                    int64_t timestamp);
    void PostEvents(const std::vector<Event>& events, bool wakeup);
    void PostFlushes(const std::vector<int32_t>& handles);
    void WriteSamplingFrequency(IioDeviceState* device, const std::vector<IioSensorData*>& active);

    std::string ReadString(const std::string& path, const std::string& fallback = "") const;
    double ReadDouble(const std::string& path, double fallback) const;
    int ReadInt(const std::string& path, int fallback) const;
    double ReadSharedAttribute(const std::string& path, const std::string& channel,
                               const std::string& suffix, double fallback) const;
    bool WriteString(const std::string& path, const std::string& value) const;
    bool Exists(const std::string& path) const;
    std::string TypePrefix(SensorType type) const;
    iio::Unit UnitForType(SensorType type) const;
    bool IsVector(SensorType type) const;
    bool IsOnChange(SensorType type) const;
    int64_t BoottimeNs() const;

    std::map<int32_t, std::shared_ptr<IioSensorData>> sensors_;
    std::map<int, std::shared_ptr<IioDeviceState>> devices_;
    std::unique_ptr<SensorHwdb> sensor_hwdb_;
    int32_t next_handle_ = 1;
    PostEventsCallback callback_;
    std::atomic<OperationMode> operation_mode_{OperationMode::NORMAL};
    std::mutex callback_mutex_;
    std::mutex mutex_;
};

extern "C" ISensorBackend* CreateSensorBackend();

}  // namespace aidl::android::hardware::sensors::mainline
