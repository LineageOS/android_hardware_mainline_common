/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsInputBackend"

#include "InputBackend.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>

namespace aidl::android::hardware::sensors::mainline {

static constexpr const char* kInputBasePath = "/dev/input";
static constexpr int32_t kDefaultMaxDelayUs = 10 * 1000 * 1000;
static constexpr int64_t kNanosecondsPerSecond = 1000LL * 1000 * 1000;

static constexpr float kDefaultAccelScale = 9.81f / 256.0f;

extern "C" __attribute__((visibility("default"))) ISensorBackend* CreateSensorBackend() {
    return new InputBackend();
}

InputBackend::InputBackend() = default;

InputBackend::~InputBackend() {
    Deinitialize();
}

std::string InputBackend::GetName() const {
    return "Input";
}

std::string InputBackend::ReadSysfsString(const std::string& path,
                                          const std::string& default_value) {
    std::string result;
    if (!::android::base::ReadFileToString(path, &result)) {
        return default_value;
    }
    if (result.empty()) return "";
    if (result.back() == '\0' || result.back() == '\x0a') result.pop_back();
    return ::android::base::Trim(result);
}

bool InputBackend::HasSwitchCapability(const std::string& sysfs_path) {
    std::string caps_path = sysfs_path + "/capabilities/sw";
    std::string content = ReadSysfsString(caps_path, "");
    if (content.empty()) {
        return false;
    }

    unsigned long long sw_mask = 0;
    char* end = nullptr;
    sw_mask = std::strtoull(content.c_str(), &end, 16);
    if (end == content.c_str() || *end != '\0') {
        return false;
    }

    return (sw_mask & (1ULL << SW_FRONT_PROXIMITY)) != 0;
}

bool InputBackend::HasAbsoluteAxes(const std::string& sysfs_path) {
    std::string caps_path = sysfs_path + "/capabilities/abs";
    std::string content = ReadSysfsString(caps_path, "");
    if (content.empty()) {
        return false;
    }

    unsigned long long abs_mask = 0;
    char* end = nullptr;
    abs_mask = std::strtoull(content.c_str(), &end, 16);
    if (end == content.c_str() || *end != '\0') {
        return false;
    }

    bool has_xyz = ((abs_mask & (1ULL << ABS_X)) != 0) && ((abs_mask & (1ULL << ABS_Y)) != 0) &&
                   ((abs_mask & (1ULL << ABS_Z)) != 0);
    return has_xyz;
}

bool InputBackend::CheckInputDeviceHasSensor(const std::string& sysfs_path, int& sensor_type_out,
                                             bool& is_switch_out) {
    if (HasSwitchCapability(sysfs_path)) {
        sensor_type_out = static_cast<int>(SensorType::PROXIMITY);
        is_switch_out = true;
        return true;
    }

    if (HasAbsoluteAxes(sysfs_path)) {
        sensor_type_out = static_cast<int>(SensorType::ACCELEROMETER);
        is_switch_out = false;
        return true;
    }

    return false;
}

void InputBackend::DiscoverDevices() {
    DIR* dir = opendir(kInputBasePath);
    if (dir == nullptr) {
        LOG(WARNING) << "Cannot open " << kInputBasePath << ": " << strerror(errno);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("event") != 0) {
            continue;
        }

        std::string device_path = std::string(kInputBasePath) + "/" + name;

        ::android::base::unique_fd fd(open(device_path.c_str(), O_RDONLY | O_NONBLOCK));
        if (fd.get() < 0) {
            continue;
        }

        char name_buf[256] = {0};
        if (ioctl(fd.get(), EVIOCGNAME(sizeof(name_buf)), name_buf) < 0) {
            continue;
        }

        std::string device_name = name_buf;

        std::string sysfs_base = "/sys/class/input/" + name;
        struct stat st;
        if (stat(sysfs_base.c_str(), &st) != 0) {
            continue;
        }

        int sensor_type = -1;
        bool is_switch = false;
        if (!CheckInputDeviceHasSensor(sysfs_base, sensor_type, is_switch)) {
            continue;
        }

        auto sensor = std::make_unique<InputSensorData>();
        sensor->handle = next_handle_++;
        sensor->device_path = device_path;
        sensor->device_name = device_name;
        sensor->type = static_cast<SensorType>(sensor_type);
        sensor->is_switch = is_switch;
        sensor->enabled = false;
        sensor->sampling_period_ns = 200 * 1000 * 1000;
        sensor->stop_thread = false;
        sensor->last_value = 0.0f;
        sensor->accel_scale = kDefaultAccelScale;

        sensor->sensor_info.sensorHandle = sensor->handle;
        sensor->sensor_info.name = device_name;
        sensor->sensor_info.vendor = "Linux Input";
        sensor->sensor_info.version = 1;
        sensor->sensor_info.type = sensor->type;
        sensor->sensor_info.typeAsString = "";
        sensor->sensor_info.fifoReservedEventCount = 0;
        sensor->sensor_info.fifoMaxEventCount = 0;
        sensor->sensor_info.requiredPermission = "";

        switch (sensor->type) {
            case SensorType::ACCELEROMETER:
                sensor->sensor_info.maxRange = 78.4f;
                sensor->sensor_info.resolution = sensor->accel_scale;
                sensor->sensor_info.power = 0.13f;
                sensor->sensor_info.minDelayUs = 10000;
                sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
                sensor->sensor_info.flags =
                        static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
                break;
            case SensorType::PROXIMITY:
                sensor->sensor_info.maxRange = 5.0f;
                sensor->sensor_info.resolution = 1.0f;
                sensor->sensor_info.power = 0.012f;
                sensor->sensor_info.minDelayUs = 0;
                sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
                sensor->sensor_info.flags =
                        static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE |
                                             SensorInfo::SENSOR_FLAG_BITS_WAKE_UP);
                break;
            default:
                sensor->sensor_info.maxRange = 1.0f;
                sensor->sensor_info.resolution = 0.01f;
                sensor->sensor_info.power = 0.001f;
                sensor->sensor_info.minDelayUs = 0;
                sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
                sensor->sensor_info.flags = 0;
                break;
        }

        LoadSensorOverrides(sensor.get());

        int32_t handle = sensor->handle;
        sensors_[handle] = std::move(sensor);
        LOG(INFO) << "Input sensor discovered: " << device_name << " (handle=" << handle
                  << ", type=" << sensor_type << ", switch=" << (is_switch ? "yes" : "no") << ")";
    }

    closedir(dir);
}

int32_t InputBackend::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    post_events_callback_ = callback;

    DiscoverDevices();

    LOG(INFO) << "Input backend initialized with " << sensors_.size() << " sensor(s)";
    return 0;
}

void InputBackend::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, sensor] : sensors_) {
        sensor->enabled = false;
        sensor->stop_thread = true;
        sensor->poll_cv.notify_all();
        if (sensor->poll_thread.joinable()) {
            sensor->poll_thread.join();
        }
    }
    post_events_callback_ = nullptr;
    sensors_.clear();
    next_handle_ = 1;
    LOG(INFO) << "Input backend deinitialized";
}

void InputBackend::LoadSensorOverrides(InputSensorData* sensor) {
    if (sensor->type != SensorType::ACCELEROMETER) {
        return;
    }

    std::string prop_key =
            std::string("vendor.sensors.input.") + sensor->device_name + ".accel_scale";
    std::string value = ::android::base::GetProperty(prop_key, "");
    if (!value.empty()) {
        char* endptr;
        float scale = strtof(value.c_str(), &endptr);
        if (endptr != value.c_str() && *endptr == '\0' && scale > 0.0f) {
            sensor->accel_scale = scale;
            sensor->sensor_info.resolution = scale;
            LOG(INFO) << "Input sensor " << sensor->device_name
                      << " using custom accel_scale: " << scale;
        } else {
            LOG(WARNING) << "Failed to parse " << prop_key << ": invalid value '" << value << "'";
        }
    }
}

std::vector<SensorInfo> InputBackend::GetSensorsList() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> result;
    for (const auto& [handle, sensor] : sensors_) {
        result.push_back(sensor->sensor_info);
    }
    return result;
}

void InputBackend::PollSensorThread(InputSensorData* sensor) {
    LOG(DEBUG) << "Input poll thread started for sensor " << sensor->handle;

    while (!sensor->stop_thread.load()) {
        std::unique_lock<std::mutex> lock(sensor->poll_mutex);

        if (!sensor->enabled.load() || operation_mode_ == OperationMode::DATA_INJECTION) {
            sensor->poll_cv.wait(lock, [&] {
                return (sensor->enabled.load() && operation_mode_ == OperationMode::NORMAL) ||
                       sensor->stop_thread.load();
            });
            continue;
        }

        int64_t period_ns = sensor->sampling_period_ns;
        if (period_ns <= 0) {
            period_ns = 200 * 1000 * 1000;
        }

        auto events = ReadSensorData(sensor);
        if (!events.empty() && post_events_callback_) {
            bool wakeup = (sensor->sensor_info.flags &
                           static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP)) != 0;
            post_events_callback_(events, wakeup);
        }

        sensor->poll_cv.wait_for(lock, std::chrono::nanoseconds(period_ns));
    }

    LOG(DEBUG) << "Input poll thread stopped for sensor " << sensor->handle;
}

std::vector<Event> InputBackend::ReadSensorData(InputSensorData* sensor) {
    std::vector<Event> events;

    ::android::base::unique_fd fd(open(sensor->device_path.c_str(), O_RDONLY | O_NONBLOCK));
    if (fd.get() < 0) {
        return events;
    }

    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    int64_t timestamp = ts.tv_sec * kNanosecondsPerSecond + ts.tv_nsec;

    if (sensor->is_switch && sensor->type == SensorType::PROXIMITY) {
        unsigned char sw_bits[(SW_MAX + 7) / 8] = {0};
        if (ioctl(fd.get(), EVIOCGSW(sizeof(sw_bits)), sw_bits) < 0) {
            return events;
        }

        bool is_near = (sw_bits[SW_FRONT_PROXIMITY / 8] & (1 << (SW_FRONT_PROXIMITY % 8))) != 0;
        float value = is_near ? 0.0f : 5.0f;

        if (std::abs(value - sensor->last_value) < 0.5f) {
            return events;
        }
        sensor->last_value = value;

        Event event;
        event.sensorHandle = sensor->handle;
        event.sensorType = sensor->type;
        event.timestamp = timestamp;
        EventPayload payload;
        payload.set<EventPayload::Tag::scalar>(value);
        event.payload = payload;
        events.push_back(event);
    } else if (!sensor->is_switch && sensor->type == SensorType::ACCELEROMETER) {
        struct input_absinfo abs_x, abs_y, abs_z;
        if (ioctl(fd.get(), EVIOCGABS(ABS_X), &abs_x) < 0 ||
            ioctl(fd.get(), EVIOCGABS(ABS_Y), &abs_y) < 0 ||
            ioctl(fd.get(), EVIOCGABS(ABS_Z), &abs_z) < 0) {
            return events;
        }

        Event event;
        event.sensorHandle = sensor->handle;
        event.sensorType = sensor->type;
        event.timestamp = timestamp;

        EventPayload::Vec3 vec3 = {
                .x = static_cast<float>(abs_x.value) * sensor->accel_scale,
                .y = static_cast<float>(abs_y.value) * sensor->accel_scale,
                .z = static_cast<float>(abs_z.value) * sensor->accel_scale,
                .status = SensorStatus::ACCURACY_HIGH,
        };
        EventPayload payload;
        payload.set<EventPayload::Tag::vec3>(vec3);
        event.payload = payload;
        events.push_back(event);
    }

    return events;
}

int32_t InputBackend::Activate(int32_t sensor_handle, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sensors_.find(sensor_handle);
    if (it == sensors_.end()) {
        return -EINVAL;
    }

    auto& sensor = it->second;

    if (sensor->enabled.load() == enabled) {
        return 0;
    }

    sensor->enabled = enabled;

    if (enabled) {
        sensor->stop_thread = false;
        sensor->poll_thread = std::thread(&InputBackend::PollSensorThread, this, sensor.get());
    } else {
        sensor->stop_thread = true;
        sensor->poll_cv.notify_all();
        if (sensor->poll_thread.joinable()) {
            sensor->poll_thread.join();
        }
    }

    LOG(INFO) << "Input sensor " << sensor_handle << " " << (enabled ? "activated" : "deactivated");
    return 0;
}

int32_t InputBackend::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                            int64_t /* max_report_latency_ns */) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sensors_.find(sensor_handle);
    if (it == sensors_.end()) {
        return -EINVAL;
    }

    auto& sensor = it->second;
    int64_t min_ns = static_cast<int64_t>(sensor->sensor_info.minDelayUs) * 1000LL;
    int64_t max_ns = static_cast<int64_t>(sensor->sensor_info.maxDelayUs) * 1000LL;

    if (min_ns > 0 && sampling_period_ns < min_ns) {
        sampling_period_ns = min_ns;
    }
    if (max_ns > 0 && sampling_period_ns > max_ns) {
        sampling_period_ns = max_ns;
    }

    sensor->sampling_period_ns = sampling_period_ns;
    sensor->poll_cv.notify_all();
    return 0;
}

int32_t InputBackend::Flush(int32_t sensor_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sensors_.find(sensor_handle);
    if (it == sensors_.end()) {
        return -EINVAL;
    }

    auto& sensor = it->second;
    if (!sensor->enabled.load()) {
        return -EINVAL;
    }

    if (sensor->sensor_info.flags &
        static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ONE_SHOT_MODE)) {
        return -EINVAL;
    }

    Event ev;
    ev.sensorHandle = sensor_handle;
    ev.sensorType = SensorType::META_DATA;
    EventPayload::MetaData meta = {
            .what = EventPayload::MetaData::MetaDataEventType::META_DATA_FLUSH_COMPLETE,
    };
    ev.payload.set<EventPayload::Tag::meta>(meta);

    if (post_events_callback_) {
        post_events_callback_({ev}, false);
    }

    return 0;
}

int32_t InputBackend::SetOperationMode(OperationMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    operation_mode_ = mode;
    for (auto& [handle, sensor] : sensors_) {
        sensor->poll_cv.notify_all();
    }
    return 0;
}

}  // namespace aidl::android::hardware::sensors::mainline
