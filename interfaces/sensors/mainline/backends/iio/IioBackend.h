/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * IIO Backend Architecture
 * ========================
 *
 * This backend bridges Android's sensor HAL with the Linux IIO subsystem.
 * It discovers sensors from /sys/bus/iio/devices/ and supports three
 * operating modes:
 *
 * 1. Buffer mode with hrtimer trigger (CONFIG_IIO_CONFIGFS required)
 * 2. Buffer mode with device-provided hardware trigger (e.g., bmi160 data-ready)
 * 3. Poll mode reading _raw sysfs attributes periodically
 *
 * Multi-Sensor Device Handling
 * ----------------------------
 * Some IIO devices expose multiple sensor types on a single device node.
 * For example, the bmi160 IMU exposes both accelerometer (scan_index 7-9)
 * and gyroscope (scan_index 4-6) on one IIO device with a shared buffer.
 *
 * The backend handles this by:
 * - Creating separate IioSensorData entries for each sensor type
 * - Sharing one IioDeviceState per physical IIO device
 * - Computing scan_size from ALL channels on the device (not just one type)
 * - Each sensor stores its channel subset with correct buffer byte offsets
 * - One reader thread per device demuxes samples to all active sensors
 *
 * Trigger Setup Chain
 * -------------------
 * EnableDeviceBuffer() attempts triggers in order:
 * 1. hrtimer (software periodic trigger, needs configfs)
 * 2. Device trigger (hardware data-ready interrupt, auto-detected)
 * 3. Push-based (no trigger, driver pushes data via callbacks)
 * 4. Poll mode fallback (if all buffer attempts fail)
 *
 * Poll Mode Fallback
 * ------------------
 * When buffer enable fails, BufferSensorThread sets sensor->is_poll_mode=true.
 * PollSensorThread detects this flag change after BufferSensorThread returns
 * and continues with the poll mode loop instead of exiting.
 *
 * HID Sensor Compatibility
 * ------------------------
 * HID sensors (hid-sensor-accel-3d, hid-sensor-gyro-3d, etc.) use push-based
 * buffers via iio_push_to_buffers_with_ts() when HID reports arrive. They don't
 * follow the standard trigger-based buffer model.
 *
 * Expected behavior:
 * - Buffer enable will likely fail (scan_elements may not be writable)
 * - Falls back to poll mode automatically
 * - Poll mode reads _raw attributes which HID sensors expose
 * - Works correctly but with higher latency than buffer mode
 *
 * HID sensors are common on x86 laptops/desktops with Intel/AMD chipsets.
 */

#pragma once

#include <libsensors_mainline/SensorBackend.h>

#include <SensorHwdb.h>

#include <android-base/unique_fd.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

/*
 * Per-channel metadata parsed from scan_elements/in_*_type files.
 * The 'location' field is the byte offset in the buffer for this channel,
 * computed with proper alignment to handle mixed-size channels (e.g.,
 * 16-bit data followed by 64-bit timestamp requires 8-byte alignment).
 */
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

struct IioSensorData;

/*
 * Shared state for all sensors on one physical IIO device.
 *
 * When an IIO device exposes multiple sensor types (e.g., bmi160 accel+gyro),
 * multiple IioSensorData objects share one IioDeviceState via shared_ptr.
 *
 * Key responsibilities:
 * - Owns the buffer fd (only one open fd per device, IIO reads are destructive)
 * - Computes scan_size from ALL channels on the device (not just one sensor's)
 * - Manages the trigger (hrtimer or device-provided)
 * - Runs a single reader thread that demuxes samples to all active sensors
 * - Tracks active sensors to know when to enable/disable the device buffer
 */
struct IioDeviceState {
    int32_t dev_num;
    std::string sysfs_path;

    ::android::base::unique_fd buffer_fd;
    int signal_pipe_fd[2] = {-1, -1};
    int32_t scan_size = 0;
    std::string trigger_name;

    std::thread reader_thread;
    std::atomic_bool reader_running{false};

    std::mutex mutex;
    std::vector<IioSensorData*> active_sensors;
    std::vector<IioChannelInfo> all_channels;
};

/*
 * Per-sensor state.
 *
 * For multi-type devices, multiple IioSensorData objects may share the same
 * IioDeviceState. Each sensor stores only its own channels (subset of device
 * channels) but with the correct byte offsets into the shared device buffer.
 *
 * is_poll_mode: true for poll mode (reads _raw attributes), false for buffer
 *               mode. May change from false to true at runtime if buffer
 *               enable fails (see BufferSensorThread).
 */
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
    std::string parent_modalias;
    std::string label;

    std::shared_ptr<IioDeviceState> device_state;
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
    void ParseMountMatrix(const std::string& sysfs_path, SensorType type, float matrix[9]);
    bool ParseMountMatrixFromString(const std::string& content, float matrix[9]);
    float ReadSysfsFloat(const std::string& path, float default_value);
    int32_t ReadSysfsInt(const std::string& path, int32_t default_value);
    std::string ReadSysfsString(const std::string& path, const std::string& default_value);
    bool WriteSysfsInt(const std::string& path, int32_t value);

    void PollSensorThread(IioSensorData* sensor);
    void BufferSensorThread(IioSensorData* sensor);
    void DeviceBufferReaderThread(IioDeviceState* device);
    std::vector<Event> ReadPollSensorData(IioSensorData* sensor);
    std::vector<Event> ParseBufferSamples(IioSensorData* sensor, const uint8_t* data,
                                          size_t num_samples);
    bool EnableDeviceBuffer(IioDeviceState* device, bool enable);
    bool SetupHrtimerTrigger(IioDeviceState* device, int32_t sampling_period_ns);
    bool SetupDeviceTrigger(IioDeviceState* device, int32_t sampling_period_ns);
    void TeardownHrtimerTrigger(IioDeviceState* device);
    bool OpenDeviceBufferFd(IioDeviceState* device);
    void CloseDeviceBufferFd(IioDeviceState* device);

    std::optional<SensorType> MapIioTypeToSensorType(const std::string& iio_name);
    std::optional<SensorType> ClassifyChannelByName(const std::string& channel_name);
    std::optional<SensorType> DetectTypeFromScanElements(const std::string& sysfs_path);
    std::optional<SensorType> DetectTypeFromSysfsAttributes(const std::string& sysfs_path);
    std::string ParseVendorFromCompatible(const std::string& of_compatible);
    bool IsVec3Type(SensorType type);
    std::string GetIioTypePrefix(SensorType type);
    float ReadSharedAttribute(const std::string& sysfs_path, const std::string& channel_name,
                              const std::string& suffix, float default_value);

    void DeriveSensorInfoFromSysfs(IioSensorData* sensor);
    void ApplyHwdbProperties(IioSensorData* sensor);
    void ApplySensorInfoOverrides(IioSensorData* sensor);
    std::vector<float> ReadAvailableFrequencies(const std::string& sysfs_path, SensorType type);

    EventPayload::Vec3 BuildVec3Value(const std::vector<float>& values);

    std::map<int32_t, std::unique_ptr<IioSensorData>> sensors_;
    std::map<int32_t, std::shared_ptr<IioDeviceState>> device_states_;
    std::unique_ptr<SensorHwdb> sensor_hwdb_;
    int32_t next_handle_ = 1;
    PostEventsCallback post_events_callback_;
    OperationMode operation_mode_ = OperationMode::NORMAL;
    std::mutex mutex_;
};

extern "C" ISensorBackend* CreateSensorBackend();

}  // namespace aidl::android::hardware::sensors::mainline
