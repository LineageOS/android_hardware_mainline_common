/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_common/MountMatrix.h>
#include <libsensors_common/PeriodicWorker.h>
#include <libsensors_hwdb/SensorHwdb.h>
#include <libsensors_mainline/SensorBackend.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "IioChannel.h"
#include "IioTypes.h"

namespace aidl::android::hardware::sensors::mainline {

// Static description of the IIO device a sensor belongs to, shared by all its
// sensors.
struct IioDeviceInfo {
    int dev_num = -1;
    std::string sysfs_path;
    std::string name;            // "name" attribute
    std::string label;           // "label" attribute
    std::string of_name;         // of_node/name
    std::string compatible;      // of_node/compatible (first entry)
    std::string modalias;        // ../modalias
    std::string model;           // best human readable model name
    std::string config_key;      // "iio.<sanitized model>"
    std::string alt_config_key;  // "iio.<sanitized name>" when different, else empty
    IioDeviceQuirks quirks;
    bool buffer_capable = false;

    // Configuration keys for a device level setting, most specific first.
    std::vector<std::string> ConfigKeys(const std::string& key) const {
        std::vector<std::string> keys = {config_key + "." + key};
        if (!alt_config_key.empty()) {
            keys.push_back(alt_config_key + "." + key);
        }
        return keys;
    }
};

/*
 * One Android sensor backed by a group of channels of an IIO device.
 *
 * The class knows how to derive the SensorInfo from sysfs, how to convert raw
 * channel values into an Android event (scale, offset, unit, mount matrix,
 * proximity near level) and implements the per-sensor filtering (rate
 * decimation in buffer mode, on-change de-duplication).
 *
 * Thread-safety: the event producing methods (BuildEventFromScan, PollOnce,
 * Filter) are always called from a single producer thread at a time (the
 * device reader thread in buffer mode or the sensor poll worker in poll mode).
 * Activation state is managed by the owning IioDevice under its lock.
 */
class IioSensor {
  public:
    IioSensor(const IioDeviceInfo& device, int32_t handle, const IioSensorSpec& spec,
              std::vector<IioChannel*> channels, const SensorHwdb* hwdb);

    const SensorInfo& GetInfo() const { return info_; }
    SensorInfo* MutableInfo() { return &info_; }
    int32_t GetHandle() const { return info_.sensorHandle; }
    const IioSensorSpec& GetSpec() const { return spec_; }
    const std::vector<IioChannel*>& GetChannels() const { return channels_; }
    const MountMatrix& GetMountMatrix() const { return mount_matrix_; }

    // Whether the sensor can be served through the device buffer / by polling
    // sysfs attributes.
    bool CanBuffer() const { return can_buffer_; }
    bool CanPoll() const { return can_poll_; }

    // Activation state, managed by IioDevice.
    bool IsActive() const { return active_.load(); }
    void SetActive(bool active);
    int64_t GetPeriodNs() const { return period_ns_.load(); }
    void SetPeriodNs(int64_t period_ns);
    double GetRequestedFrequencyHz() const;

    // Buffer mode: builds the event for one scan. Returns nullopt if the
    // channels of the sensor are not part of the scan.
    std::optional<Event> BuildEventFromScan(const uint8_t* scan, int64_t timestamp_ns);

    // Poll mode: reads the sysfs attributes and builds an event.
    std::optional<Event> ReadEventFromSysfs(int64_t timestamp_ns);

    // Applies decimation / on-change filtering. Returns the event to post, or
    // nullopt to drop it. Must be called from the producer thread.
    std::optional<Event> Filter(const Event& event);

    // Resets the filter state (call on activation).
    void ResetFilterState();

    // Poll worker, owned here but driven by IioDevice.
    PeriodicWorker* GetPollWorker() { return poll_worker_.get(); }
    void SetPollWorker(std::unique_ptr<PeriodicWorker> worker) { poll_worker_ = std::move(worker); }

    // Poll period for on-change sensors is bounded so that sysfs polling does
    // not run wild.
    int64_t GetPollPeriodNs() const;

    std::string Describe() const;

  private:
    void DeriveSensorInfo();
    void ApplyConfigOverrides();
    void ResolveMountMatrix();
    void ResolveProximityNearLevel();
    std::optional<std::string> GetSetting(const std::string& key) const;
    std::vector<std::string> SettingKeys(const std::string& key) const;

    double ConvertChannelValue(const IioChannel& channel, double raw) const;
    std::optional<Event> BuildEvent(const std::vector<double>& values, int64_t timestamp_ns);
    float ConvertProximity(double value);

    const IioDeviceInfo device_;
    const IioSensorSpec spec_;
    const std::vector<IioChannel*> channels_;
    const SensorHwdb* hwdb_;

    SensorInfo info_;
    MountMatrix mount_matrix_;
    bool can_buffer_ = false;
    bool can_poll_ = false;

    // Proximity handling.
    double proximity_near_level_ = 0.0;
    bool proximity_is_near_ = false;

    // Activation (read from producer threads, written under the device lock).
    std::atomic<bool> active_{false};
    std::atomic<int64_t> period_ns_;

    // Filtering state (producer thread only).
    int64_t last_emit_ns_ = 0;
    std::optional<Event> last_event_;

    std::unique_ptr<PeriodicWorker> poll_worker_;
};

}  // namespace aidl::android::hardware::sensors::mainline
