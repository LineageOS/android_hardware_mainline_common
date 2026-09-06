/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <android-base/unique_fd.h>
#include <libsensors_common/MountMatrix.h>
#include <libsensors_hwdb/SensorHwdb.h>
#include <libsensors_mainline/SensorBackend.h>

#include <linux/input.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

/*
 * One /dev/input/eventN node exposing sensor-like capabilities.
 *
 * Supported sensors:
 *  - Accelerometer: EV_ABS with ABS_X/ABS_Y/ABS_Z on a device that is not a
 *    touch or game controller (or that sets INPUT_PROP_ACCELEROMETER).
 *  - Proximity: EV_SW with SW_FRONT_PROXIMITY (typically a gpio-keys node).
 *
 * The event node is only kept open while at least one of its sensors is
 * active, so that polled kernel drivers stay idle otherwise. A reader thread
 * turns evdev reports into sensor events.
 */
class InputDevice {
  public:
    enum class Kind {
        kAccelerometer,
        kProximitySwitch,
    };

    struct Sensor {
        Kind kind;
        SensorInfo info;
        bool active = false;
        int64_t period_ns;
        int64_t last_emit_ns = 0;
        // Accelerometer only.
        float lsb_per_g = 0.0f;
        MountMatrix mount_matrix;
        // Latest value of each axis (raw counts).
        int32_t raw[3] = {0, 0, 0};
        bool raw_dirty = false;
        std::optional<Event> last_event;
    };

    // Probes /dev/input/<event_name>; returns nullptr if the node does not
    // provide any supported sensor. Handles are allocated from *next_handle.
    static std::unique_ptr<InputDevice> Probe(const std::string& event_name, int32_t* next_handle,
                                              const SensorHwdb* hwdb);

    ~InputDevice();

    InputDevice(const InputDevice&) = delete;
    InputDevice& operator=(const InputDevice&) = delete;

    const std::string& GetName() const { return name_; }
    const std::string& GetPath() const { return dev_path_; }
    std::vector<SensorInfo> GetSensorInfos() const;
    bool HasSensor(int32_t handle) const;

    void SetCallback(PostEventsCallback callback);
    int32_t Activate(int32_t handle, bool enabled);
    int32_t Batch(int32_t handle, int64_t period_ns);
    void SetPaused(bool paused);
    void Shutdown();

  private:
    InputDevice(std::string event_name, std::string dev_path, std::string sysfs_path,
                std::string name);

    bool Open();
    void Close();
    void ReaderThread();
    void HandleSyncReport(int64_t timestamp_ns);
    void HandleSwitchEvent(int code, int value, int64_t timestamp_ns);
    void EmitInitialState();
    void PostEvent(const Event& event, bool wakeup);

    Sensor* FindSensor(int32_t handle);
    bool AnyActive() const;

    void AddAccelerometer(int32_t handle, const struct input_absinfo* abs, bool has_accel_prop,
                          const SensorHwdb* hwdb);
    void AddProximitySwitch(int32_t handle);

    const std::string event_name_;
    const std::string dev_path_;
    const std::string sysfs_path_;
    const std::string name_;
    std::string modalias_;
    std::string config_key_;

    mutable std::mutex mutex_;
    std::map<int32_t, Sensor> sensors_;
    PostEventsCallback callback_;
    std::atomic<bool> paused_{false};

    ::android::base::unique_fd fd_;
    ::android::base::unique_fd wake_pipe_read_;
    ::android::base::unique_fd wake_pipe_write_;
    std::thread reader_thread_;
    std::atomic<bool> reader_running_{false};
    bool clock_is_boottime_ = false;
};

}  // namespace aidl::android::hardware::sensors::mainline
