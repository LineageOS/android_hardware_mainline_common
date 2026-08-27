/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IioBackend.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace aidl::android::hardware::sensors::mainline {

static constexpr const char* kIioBasePath = "/sys/bus/iio/devices";
static constexpr int32_t kDefaultMaxDelayUs = 10 * 1000 * 1000;
static constexpr int64_t kNanosecondsPerSecond = 1000LL * 1000 * 1000;

extern "C" ISensorBackend* CreateSensorBackend() {
    return new IioBackend();
}

IioBackend::IioBackend() = default;

IioBackend::~IioBackend() {
    Deinitialize();
}

std::string IioBackend::GetName() const {
    return "IIO";
}

std::string IioBackend::ReadSysfsString(const std::string& path,
                                         const std::string& default_value) {
    std::string result;
    if (!::android::base::ReadFileToString(path, &result)) {
        return default_value;
    }
    return ::android::base::Trim(result);
}

float IioBackend::ReadSysfsFloat(const std::string& path, float default_value) {
    std::string content = ReadSysfsString(path, "");
    if (content.empty()) {
        return default_value;
    }
    char* end = nullptr;
    float result = std::strtof(content.c_str(), &end);
    if (end == content.c_str() || *end != '\0') {
        return default_value;
    }
    return result;
}

int32_t IioBackend::ReadSysfsInt(const std::string& path, int32_t default_value) {
    std::string content = ReadSysfsString(path, "");
    if (content.empty()) {
        return default_value;
    }
    char* end = nullptr;
    long result = std::strtol(content.c_str(), &end, 10);
    if (end == content.c_str() || *end != '\0') {
        return default_value;
    }
    return static_cast<int32_t>(result);
}

bool IioBackend::WriteSysfsInt(const std::string& path, int32_t value) {
    return ::android::base::WriteStringToFile(std::to_string(value), path);
}

void IioBackend::ParseMountMatrix(const std::string& sysfs_path, float matrix[9]) {
    for (int i = 0; i < 9; i++) {
        matrix[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    }

    std::vector<std::string> candidates = {
            sysfs_path + "/mount_matrix",
            sysfs_path + "/in_accel_mount_matrix",
            sysfs_path + "/in_mount_matrix",
    };

    for (const auto& path : candidates) {
        std::string content = ReadSysfsString(path, "");
        if (content.empty()) {
            continue;
        }

        auto rows = ::android::base::Split(content, ";");
        if (rows.size() != 3) {
            continue;
        }

        bool valid = true;
        float parsed[9];
        for (int r = 0; r < 3; r++) {
            auto cols = ::android::base::Split(rows[r], ",");
            if (cols.size() != 3) {
                valid = false;
                break;
            }
            for (int c = 0; c < 3; c++) {
                std::string trimmed = ::android::base::Trim(cols[c]);
                char* end = nullptr;
                float val = std::strtof(trimmed.c_str(), &end);
                if (end == trimmed.c_str() || *end != '\0') {
                    valid = false;
                    break;
                }
                parsed[r * 3 + c] = val;
            }
            if (!valid) break;
        }

        if (valid) {
            bool all_zero = true;
            for (int i = 0; i < 9; i++) {
                if (parsed[i] != 0.0f) {
                    all_zero = false;
                    break;
                }
            }
            if (!all_zero) {
                std::copy(parsed, parsed + 9, matrix);
                LOG(DEBUG) << "Mount matrix loaded from " << path;
                return;
            }
        }
    }
}

bool IioBackend::ParseChannelType(const std::string& type_str, IioChannelInfo& channel) {
    char endianness;
    char sign;
    int realbits, storagebits, shift;

    if (sscanf(type_str.c_str(), "%ce:%c%u/%u>>%u", &endianness, &sign, &realbits, &storagebits,
               &shift) != 5) {
        return false;
    }

    channel.is_big_endian = (endianness == 'b');
    channel.sign = sign;
    channel.realbits = static_cast<uint8_t>(realbits);
    channel.storagebits = static_cast<uint8_t>(storagebits);
    channel.shift = static_cast<uint8_t>(shift);
    return true;
}

int32_t IioBackend::MapIioTypeToSensorType(const std::string& iio_name) {
    if (iio_name.find("accel") != std::string::npos) return static_cast<int32_t>(SensorType::ACCELEROMETER);
    if (iio_name.find("gyro") != std::string::npos ||
        iio_name.find("anglvel") != std::string::npos)
        return static_cast<int32_t>(SensorType::GYROSCOPE);
    if (iio_name.find("magn") != std::string::npos)
        return static_cast<int32_t>(SensorType::MAGNETIC_FIELD);
    if (iio_name.find("light") != std::string::npos ||
        iio_name.find("illuminance") != std::string::npos ||
        iio_name.find("intensity") != std::string::npos)
        return static_cast<int32_t>(SensorType::LIGHT);
    if (iio_name.find("proximity") != std::string::npos)
        return static_cast<int32_t>(SensorType::PROXIMITY);
    if (iio_name.find("temp") != std::string::npos)
        return static_cast<int32_t>(SensorType::AMBIENT_TEMPERATURE);
    if (iio_name.find("pressure") != std::string::npos ||
        iio_name.find("baro") != std::string::npos)
        return static_cast<int32_t>(SensorType::PRESSURE);
    if (iio_name.find("humidity") != std::string::npos)
        return static_cast<int32_t>(SensorType::RELATIVE_HUMIDITY);
    return -1;
}

bool IioBackend::IsVec3Type(SensorType type) {
    return type == SensorType::ACCELEROMETER || type == SensorType::GYROSCOPE ||
           type == SensorType::MAGNETIC_FIELD;
}

bool IioBackend::IsOnChangeType(SensorType type) {
    return type == SensorType::LIGHT || type == SensorType::PROXIMITY ||
           type == SensorType::AMBIENT_TEMPERATURE || type == SensorType::RELATIVE_HUMIDITY ||
           type == SensorType::PRESSURE;
}

void IioBackend::DiscoverDevices() {
    DIR* dir = opendir(kIioBasePath);
    if (dir == nullptr) {
        LOG(WARNING) << "Cannot open " << kIioBasePath << ": " << strerror(errno);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find("iio:device") == std::string::npos) {
            continue;
        }

        int dev_num = -1;
        if (sscanf(name.c_str(), "iio:device%d", &dev_num) != 1) {
            continue;
        }

        std::string sysfs_path = std::string(kIioBasePath) + "/" + name;
        DiscoverSensors(dev_num, sysfs_path);
    }

    closedir(dir);
}

void IioBackend::DiscoverSensors(int dev_num, const std::string& sysfs_path) {
    std::string device_name = ReadSysfsString(sysfs_path + "/name", "unknown");
    LOG(INFO) << "IIO device " << dev_num << ": " << device_name << " at " << sysfs_path;

    int32_t sensor_type = MapIioTypeToSensorType(device_name);
    if (sensor_type < 0) {
        std::string scan_dir = sysfs_path + "/scan_elements";
        DIR* scan_dp = opendir(scan_dir.c_str());
        if (scan_dp == nullptr) {
            LOG(DEBUG) << "No scan_elements for " << device_name << ", skipping";
            return;
        }

        struct dirent* scan_entry;
        while ((scan_entry = readdir(scan_dp)) != nullptr) {
            std::string fname = scan_entry->d_name;
            if (fname.size() > 3 && fname.substr(fname.size() - 3) == "_en") {
                std::string prefix = fname.substr(0, fname.size() - 3);
                if (prefix.find("in_") == 0) {
                    prefix = prefix.substr(3);
                }
                auto pos = prefix.find_last_of('_');
                if (pos != std::string::npos) {
                    std::string base = prefix.substr(0, pos);
                    sensor_type = MapIioTypeToSensorType(base);
                    if (sensor_type >= 0) break;
                }
                sensor_type = MapIioTypeToSensorType(prefix);
                if (sensor_type >= 0) break;
            }
        }
        closedir(scan_dp);
    }

    if (sensor_type < 0) {
        std::string raw_test = sysfs_path + "/in_illuminance_raw";
        struct stat st;
        if (stat(raw_test.c_str(), &st) == 0) {
            sensor_type = static_cast<int32_t>(SensorType::LIGHT);
        } else {
            raw_test = sysfs_path + "/in_proximity_raw";
            if (stat(raw_test.c_str(), &st) == 0) {
                sensor_type = static_cast<int32_t>(SensorType::PROXIMITY);
            }
        }
    }

    if (sensor_type < 0) {
        LOG(DEBUG) << "IIO device " << dev_num << " has no recognized sensor type, skipping";
        return;
    }

    auto sensor = std::make_unique<IioSensorData>();
    sensor->handle = next_handle_++;
    sensor->sysfs_path = sysfs_path;
    sensor->device_name = device_name;
    sensor->type = static_cast<SensorType>(sensor_type);
    sensor->is_poll_mode = true;
    sensor->enabled = false;
    sensor->sampling_period_ns = 200 * 1000 * 1000;
    sensor->stop_thread = false;
    sensor->dev_num = dev_num;

    ParseMountMatrix(sysfs_path, sensor->mount_matrix);

    bool has_scan_elements = false;
    std::string scan_dir = sysfs_path + "/scan_elements";
    DIR* scan_dp = opendir(scan_dir.c_str());
    if (scan_dp != nullptr) {
        struct dirent* scan_entry;
        while ((scan_entry = readdir(scan_dp)) != nullptr) {
            std::string fname = scan_entry->d_name;
            if (fname.size() > 3 && fname.substr(fname.size() - 3) == "_en") {
                has_scan_elements = true;
                std::string en_content = ReadSysfsString(scan_dir + "/" + fname, "0");
                if (en_content != "1") {
                    continue;
                }

                std::string chan_prefix = fname.substr(0, fname.size() - 3);

                IioChannelInfo channel;
                channel.name = chan_prefix;

                std::string type_content =
                        ReadSysfsString(scan_dir + "/" + chan_prefix + "_type", "");
                if (type_content.empty()) {
                    continue;
                }

                if (!ParseChannelType(type_content, channel)) {
                    continue;
                }

                channel.index = ReadSysfsInt(scan_dir + "/" + chan_prefix + "_index", -1);
                if (channel.index < 0) {
                    continue;
                }

                std::string scale_path = sysfs_path + "/" + chan_prefix + "_scale";
                channel.scale = ReadSysfsFloat(scale_path, 1.0f);

                std::string offset_path = sysfs_path + "/" + chan_prefix + "_offset";
                channel.offset = ReadSysfsFloat(offset_path, 0.0f);

                int32_t storage_bytes = (channel.storagebits + 7) / 8;
                channel.location = channel.index * storage_bytes;

                sensor->channels.push_back(channel);
            }
        }
        closedir(scan_dp);
    }

    if (!has_scan_elements || sensor->channels.empty()) {
        sensor->channels.clear();
        sensor->is_poll_mode = true;

        if (IsVec3Type(sensor->type)) {
            std::vector<std::string> axes = {"x", "y", "z"};
            std::string type_prefix;
            if (sensor->type == SensorType::ACCELEROMETER)
                type_prefix = "accel";
            else if (sensor->type == SensorType::GYROSCOPE)
                type_prefix = "anglvel";
            else if (sensor->type == SensorType::MAGNETIC_FIELD)
                type_prefix = "magn";

            for (size_t i = 0; i < axes.size(); i++) {
                std::string raw_path =
                        sysfs_path + "/in_" + type_prefix + "_" + axes[i] + "_raw";
                struct stat st;
                if (stat(raw_path.c_str(), &st) != 0) {
                    LOG(WARNING) << "Missing " << raw_path << " for sensor " << device_name;
                    return;
                }

                IioChannelInfo channel;
                channel.name = "in_" + type_prefix + "_" + axes[i];
                channel.index = static_cast<int32_t>(i);
                channel.sign = 's';
                channel.realbits = 16;
                channel.storagebits = 16;
                channel.shift = 0;
                channel.is_big_endian = false;
                channel.location = 0;

                std::string scale_path = sysfs_path + "/in_" + type_prefix + "_scale";
                channel.scale = ReadSysfsFloat(scale_path, 1.0f);
                channel.offset = 0.0f;

                sensor->channels.push_back(channel);
            }
        } else {
            std::string type_prefix;
            if (sensor->type == SensorType::LIGHT)
                type_prefix = "illuminance";
            else if (sensor->type == SensorType::PROXIMITY)
                type_prefix = "proximity";
            else if (sensor->type == SensorType::AMBIENT_TEMPERATURE)
                type_prefix = "temp";
            else if (sensor->type == SensorType::PRESSURE)
                type_prefix = "pressure";
            else if (sensor->type == SensorType::RELATIVE_HUMIDITY)
                type_prefix = "humidityrelative";

            std::vector<std::string> suffixes = {"_input", "_raw"};
            std::string found_path;
            for (const auto& suffix : suffixes) {
                std::string test = sysfs_path + "/in_" + type_prefix + suffix;
                struct stat st;
                if (stat(test.c_str(), &st) == 0) {
                    found_path = test;
                    break;
                }
            }

            if (found_path.empty()) {
                for (const auto& suffix : suffixes) {
                    std::string test = sysfs_path + "/in_" + type_prefix + "0" + suffix;
                    struct stat st;
                    if (stat(test.c_str(), &st) == 0) {
                        found_path = test;
                        break;
                    }
                }
            }

            if (found_path.empty()) {
                LOG(WARNING) << "No readable attribute for sensor " << device_name;
                return;
            }

            IioChannelInfo channel;
            channel.name = found_path.substr(sysfs_path.size() + 1);
            channel.index = 0;
            channel.sign = 'u';
            channel.realbits = 32;
            channel.storagebits = 32;
            channel.shift = 0;
            channel.is_big_endian = false;
            channel.location = 0;

            std::string scale_path = sysfs_path + "/in_" + type_prefix + "_scale";
            channel.scale = ReadSysfsFloat(scale_path, 1.0f);
            channel.offset = 0.0f;

            sensor->channels.push_back(channel);
        }
    } else {
        std::string dev_path = "/dev/iio:device" + std::to_string(dev_num);
        struct stat st;
        if (stat(dev_path.c_str(), &st) == 0) {
            sensor->is_poll_mode = false;
        }
    }

    std::sort(sensor->channels.begin(), sensor->channels.end(),
              [](const IioChannelInfo& a, const IioChannelInfo& b) {
                  return a.index < b.index;
              });

    sensor->sensor_info.sensorHandle = sensor->handle;
    sensor->sensor_info.name = device_name;
    sensor->sensor_info.vendor = "Linux IIO";
    sensor->sensor_info.version = 1;
    sensor->sensor_info.type = sensor->type;
    sensor->sensor_info.typeAsString = "";
    sensor->sensor_info.fifoReservedEventCount = 0;
    sensor->sensor_info.fifoMaxEventCount = 0;
    sensor->sensor_info.requiredPermission = "";

    switch (sensor->type) {
        case SensorType::ACCELEROMETER:
            sensor->sensor_info.maxRange = 78.4f;
            sensor->sensor_info.resolution = 0.001f;
            sensor->sensor_info.power = 0.13f;
            sensor->sensor_info.minDelayUs = 2500;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
            break;
        case SensorType::GYROSCOPE:
            sensor->sensor_info.maxRange = 1000.0f * static_cast<float>(M_PI) / 180.0f;
            sensor->sensor_info.resolution = 1000.0f * static_cast<float>(M_PI) / (180.0f * 32768.0f);
            sensor->sensor_info.power = 6.1f;
            sensor->sensor_info.minDelayUs = 2500;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
            break;
        case SensorType::MAGNETIC_FIELD:
            sensor->sensor_info.maxRange = 1300.0f;
            sensor->sensor_info.resolution = 0.01f;
            sensor->sensor_info.power = 0.35f;
            sensor->sensor_info.minDelayUs = 10000;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
            break;
        case SensorType::LIGHT:
            sensor->sensor_info.maxRange = 43000.0f;
            sensor->sensor_info.resolution = 10.0f;
            sensor->sensor_info.power = 0.175f;
            sensor->sensor_info.minDelayUs = 0;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
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
        case SensorType::AMBIENT_TEMPERATURE:
            sensor->sensor_info.maxRange = 80.0f;
            sensor->sensor_info.resolution = 0.01f;
            sensor->sensor_info.power = 0.001f;
            sensor->sensor_info.minDelayUs = 0;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
            break;
        case SensorType::PRESSURE:
            sensor->sensor_info.maxRange = 1100.0f;
            sensor->sensor_info.resolution = 0.005f;
            sensor->sensor_info.power = 0.004f;
            sensor->sensor_info.minDelayUs = 10000;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags = 0;
            break;
        case SensorType::RELATIVE_HUMIDITY:
            sensor->sensor_info.maxRange = 100.0f;
            sensor->sensor_info.resolution = 0.1f;
            sensor->sensor_info.power = 0.001f;
            sensor->sensor_info.minDelayUs = 0;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
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

    int32_t handle = sensor->handle;
    sensors_[handle] = std::move(sensor);
    LOG(INFO) << "IIO sensor discovered: " << device_name << " (handle=" << handle
              << ", type=" << sensor_type << ", poll_mode="
              << (sensors_[handle]->is_poll_mode ? "yes" : "no") << ")";
}

int32_t IioBackend::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    post_events_callback_ = callback;

    DiscoverDevices();

    LOG(INFO) << "IIO backend initialized with " << sensors_.size() << " sensor(s)";
    return 0;
}

void IioBackend::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, sensor] : sensors_) {
        if (sensor->enabled.load()) {
            sensor->enabled = false;
            if (!sensor->is_poll_mode) {
                EnableRingBuffer(sensor.get(), false);
            }
        }
        sensor->stop_thread = true;
        sensor->poll_cv.notify_all();
        if (sensor->poll_thread.joinable()) {
            sensor->poll_thread.join();
        }
    }
    post_events_callback_ = nullptr;
    LOG(INFO) << "IIO backend deinitialized";
}

std::vector<SensorInfo> IioBackend::GetSensorsList() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> result;
    for (const auto& [handle, sensor] : sensors_) {
        result.push_back(sensor->sensor_info);
    }
    return result;
}

void IioBackend::PollSensorThread(IioSensorData* sensor) {
    LOG(DEBUG) << "Poll thread started for sensor " << sensor->handle;

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

        auto events = ReadPollSensorData(sensor);
        if (!events.empty() && post_events_callback_) {
            bool wakeup =
                    (sensor->sensor_info.flags &
                     static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP)) != 0;
            post_events_callback_(events, wakeup);
        }

        sensor->poll_cv.wait_for(lock, std::chrono::nanoseconds(period_ns));
    }

    LOG(DEBUG) << "Poll thread stopped for sensor " << sensor->handle;
}

std::vector<Event> IioBackend::ReadPollSensorData(IioSensorData* sensor) {
    std::vector<Event> events;

    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    int64_t timestamp = ts.tv_sec * kNanosecondsPerSecond + ts.tv_nsec;

    if (IsVec3Type(sensor->type)) {
        if (sensor->channels.size() < 3) {
            return events;
        }

        std::vector<float> raw_values(3);

        for (size_t i = 0; i < 3; i++) {
            std::string path = sensor->sysfs_path + "/" + sensor->channels[i].name + "_raw";
            int32_t raw = ReadSysfsInt(path, 0);
            raw_values[i] = (static_cast<float>(raw) + sensor->channels[i].offset) *
                            sensor->channels[i].scale;
        }

        float corrected[3];
        for (int r = 0; r < 3; r++) {
            corrected[r] = sensor->mount_matrix[r * 3 + 0] * raw_values[0] +
                           sensor->mount_matrix[r * 3 + 1] * raw_values[1] +
                           sensor->mount_matrix[r * 3 + 2] * raw_values[2];
        }

        Event event;
        event.sensorHandle = sensor->handle;
        event.sensorType = sensor->type;
        event.timestamp = timestamp;

        if (sensor->type == SensorType::ACCELEROMETER) {
            corrected[0] *= 9.81f;
            corrected[1] *= 9.81f;
            corrected[2] *= 9.81f;
        }

        EventPayload::Vec3 vec3 = BuildVec3Value(
                {corrected[0], corrected[1], corrected[2]});
        event.payload.set<EventPayload::Tag::vec3>(vec3);
        events.push_back(event);
    } else {
        if (sensor->channels.empty()) {
            return events;
        }

        std::string path = sensor->sysfs_path + "/" + sensor->channels[0].name;
        float raw_value = ReadSysfsFloat(path, 0.0f);
        float value = (raw_value + sensor->channels[0].offset) * sensor->channels[0].scale;

        if (sensor->type == SensorType::AMBIENT_TEMPERATURE) {
            value /= 1000.0f;
        }

        Event event;
        event.sensorHandle = sensor->handle;
        event.sensorType = sensor->type;
        event.timestamp = timestamp;
        event.payload.set<EventPayload::Tag::scalar>(value);
        events.push_back(event);
    }

    return events;
}

void IioBackend::EnableRingBuffer(IioSensorData* sensor, bool enable) {
    std::string buffer_path = sensor->sysfs_path + "/buffer/enable";
    std::string length_path = sensor->sysfs_path + "/buffer/length";

    if (enable) {
        WriteSysfsInt(length_path, 128);
        WriteSysfsInt(buffer_path, 1);
    } else {
        WriteSysfsInt(buffer_path, 0);
    }
}

std::vector<Event> IioBackend::ReadBufferSensorData(IioSensorData* sensor) {
    std::vector<Event> events;

    std::string dev_path = "/dev/iio:device" + std::to_string(sensor->dev_num);
    ::android::base::unique_fd fd(open(dev_path.c_str(), O_RDONLY | O_NONBLOCK));
    if (fd.get() < 0) {
        return events;
    }

    int32_t scan_size = 0;
    for (const auto& channel : sensor->channels) {
        int32_t end = channel.location + (channel.storagebits + 7) / 8;
        if (end > scan_size) {
            scan_size = end;
        }
    }

    if (scan_size <= 0) {
        return events;
    }

    constexpr int kMaxSamples = 127;
    std::vector<uint8_t> buffer(kMaxSamples * scan_size);
    ssize_t bytes_read = read(fd.get(), buffer.data(), buffer.size());
    if (bytes_read <= 0) {
        return events;
    }

    int32_t num_samples = static_cast<int32_t>(bytes_read / scan_size);
    if (num_samples <= 0) {
        return events;
    }

    uint8_t* last_sample = buffer.data() + (num_samples - 1) * scan_size;

    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    int64_t timestamp = ts.tv_sec * kNanosecondsPerSecond + ts.tv_nsec;

    if (IsVec3Type(sensor->type) && sensor->channels.size() >= 3) {
        std::vector<float> values(3);
        for (size_t i = 0; i < 3; i++) {
            const auto& channel = sensor->channels[i];
            int32_t raw_value = 0;
            int32_t storage_bytes = (channel.storagebits + 7) / 8;

            if (storage_bytes <= 2) {
                int16_t val;
                memcpy(&val, last_sample + channel.location, sizeof(val));
                raw_value = val;
            } else {
                int32_t val;
                memcpy(&val, last_sample + channel.location, sizeof(val));
                raw_value = val;
            }

            if (channel.shift > 0) {
                raw_value >>= channel.shift;
            }

            uint32_t mask = (1u << channel.realbits) - 1;
            raw_value &= mask;

            if (channel.sign == 's') {
                if (raw_value & (1u << (channel.realbits - 1))) {
                    raw_value |= ~mask;
                }
            }

            values[i] = (static_cast<float>(raw_value) + channel.offset) * channel.scale;
        }

        float corrected[3];
        for (int r = 0; r < 3; r++) {
            corrected[r] = sensor->mount_matrix[r * 3 + 0] * values[0] +
                           sensor->mount_matrix[r * 3 + 1] * values[1] +
                           sensor->mount_matrix[r * 3 + 2] * values[2];
        }

        if (sensor->type == SensorType::ACCELEROMETER) {
            corrected[0] *= 9.81f;
            corrected[1] *= 9.81f;
            corrected[2] *= 9.81f;
        }

        Event event;
        event.sensorHandle = sensor->handle;
        event.sensorType = sensor->type;
        event.timestamp = timestamp;
        EventPayload::Vec3 vec3 = BuildVec3Value(
                {corrected[0], corrected[1], corrected[2]});
        event.payload.set<EventPayload::Tag::vec3>(vec3);
        events.push_back(event);
    }

    return events;
}

int32_t IioBackend::Activate(int32_t sensor_handle, bool enabled) {
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

        if (!sensor->is_poll_mode) {
            EnableRingBuffer(sensor.get(), true);
        }

        sensor->poll_thread = std::thread(&IioBackend::PollSensorThread, this, sensor.get());
    } else {
        if (!sensor->is_poll_mode) {
            EnableRingBuffer(sensor.get(), false);
        }

        sensor->stop_thread = true;
        sensor->poll_cv.notify_all();
        if (sensor->poll_thread.joinable()) {
            sensor->poll_thread.join();
        }
    }

    LOG(INFO) << "IIO sensor " << sensor_handle << " " << (enabled ? "activated" : "deactivated");
    return 0;
}

int32_t IioBackend::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
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

    std::string freq_path = sensor->sysfs_path + "/sampling_frequency";
    if (sampling_period_ns > 0) {
        float freq = static_cast<float>(kNanosecondsPerSecond) /
                     static_cast<float>(sampling_period_ns);
        WriteSysfsInt(freq_path, static_cast<int32_t>(freq));
    }

    return 0;
}

int32_t IioBackend::Flush(int32_t sensor_handle) {
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

int32_t IioBackend::SetOperationMode(OperationMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    operation_mode_ = mode;
    for (auto& [handle, sensor] : sensors_) {
        sensor->poll_cv.notify_all();
    }
    return 0;
}

EventPayload::Vec3 IioBackend::BuildVec3Value(const std::vector<float>& values) {
    EventPayload::Vec3 vec3 = {
            .x = values.size() > 0 ? values[0] : 0.0f,
            .y = values.size() > 1 ? values[1] : 0.0f,
            .z = values.size() > 2 ? values[2] : 0.0f,
            .status = SensorStatus::ACCURACY_HIGH,
    };
    return vec3;
}

}  // namespace aidl::android::hardware::sensors::mainline
