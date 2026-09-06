/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <android-base/unique_fd.h>
#include <libsensors_hwdb/SensorHwdb.h>
#include <libsensors_mainline/SensorBackend.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "IioChannel.h"
#include "IioSensor.h"
#include "IioTrigger.h"

namespace aidl::android::hardware::sensors::mainline {

/*
 * One /sys/bus/iio/devices/iio:deviceN.
 *
 * A device exposes one or more Android sensors (e.g. an IMU exposes an
 * accelerometer and a gyroscope). All sensors of a device share one data path:
 *
 *  - Buffer mode: the kernel ring buffer is enabled, a trigger is assigned if
 *    the device needs one (driver trigger or hrtimer), one reader thread reads
 *    /dev/iio:deviceN and demultiplexes every scan to the active sensors.
 *  - Poll mode: each active sensor periodically reads its "*_raw"/"*_input"
 *    attributes on its own worker thread.
 *
 * Buffer mode is preferred whenever every sensor of the device can use it. If
 * enabling the buffer fails, or if the buffer never delivers data (watchdog),
 * the device permanently falls back to poll mode.
 */
class IioDevice {
  public:
    // Probes the device; returns nullptr when it provides no usable sensor.
    static std::unique_ptr<IioDevice> Discover(int dev_num, const std::string& sysfs_path,
                                               const SensorHwdb* hwdb, int32_t* next_handle);

    ~IioDevice();

    IioDevice(const IioDevice&) = delete;
    IioDevice& operator=(const IioDevice&) = delete;

    const IioDeviceInfo& GetInfo() const { return info_; }
    std::vector<IioSensor*> GetSensors();
    IioSensor* FindSensor(int32_t handle);

    void SetCallback(PostEventsCallback callback);
    void SetPaused(bool paused);

    int32_t Activate(int32_t handle, bool enabled);
    int32_t SetPeriod(int32_t handle, int64_t period_ns);
    void Shutdown();

  private:
    enum class Mode { kIdle, kBuffer, kPoll };

    explicit IioDevice(IioDeviceInfo info);

    void ProbeChannels();
    void CreateSensors(const SensorHwdb* hwdb, int32_t* next_handle);
    IioChannel* GetOrCreateChannel(const IioChannelId& id);

    // Buffer mode. "Locked" methods expect mutex_ to be held.
    bool StartBufferLocked();
    void StopBuffer(std::unique_lock<std::mutex>* lock);
    void ReleaseBufferResourcesLocked();
    bool EnableScanElementsLocked();
    void ReaderThread();
    void HandleScans(const uint8_t* data, size_t count);
    int64_t TimestampFromScan(const uint8_t* scan, int64_t now_ns);
    void FallbackToPollFromReader();

    // Poll mode.
    void StartPollLocked(IioSensor* sensor);
    void StopPoll(std::unique_lock<std::mutex>* lock, IioSensor* sensor);
    void PollSensor(IioSensor* sensor);

    // Sampling rate programming.
    void ApplyRatesLocked();
    void WriteSamplingFrequency(const std::string& iio_type, double hz);
    double RoundFrequency(const std::string& available_attr, double hz);

    void PostEvents(const std::vector<Event>& events, bool wakeup);
    bool AnyActiveLocked() const;
    int64_t WatchdogTimeoutNsLocked() const;

    IioDeviceInfo info_;
    std::string scan_dir_;  // "scan_elements" or "buffer0"
    std::string dev_node_;  // /dev/iio:deviceN

    std::map<std::string, std::unique_ptr<IioChannel>> channels_;  // by IioChannelId::Key()
    std::vector<std::unique_ptr<IioSensor>> sensors_;
    IioChannel* timestamp_channel_ = nullptr;

    std::mutex mutex_;
    Mode mode_ = Mode::kIdle;
    bool buffer_failed_ = false;
    PostEventsCallback callback_;
    std::atomic<bool> paused_{false};

    // Buffer mode state.
    std::unique_ptr<IioTrigger> trigger_;
    std::vector<IioChannel*> scan_channels_;
    size_t scan_size_ = 0;
    bool timestamp_is_boottime_ = false;
    bool timestamp_warned_ = false;
    ::android::base::unique_fd fd_;
    ::android::base::unique_fd wake_pipe_read_;
    ::android::base::unique_fd wake_pipe_write_;
    std::thread reader_thread_;
    std::atomic<bool> reader_stop_{false};
    std::map<std::string, double> written_frequencies_;  // attribute -> last value
};

}  // namespace aidl::android::hardware::sensors::mainline
