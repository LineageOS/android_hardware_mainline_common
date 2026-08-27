/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsIioBackend"

#include "IioBackend.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aidl::android::hardware::sensors::mainline {

static constexpr const char* kIioBasePath = "/sys/bus/iio/devices";
static constexpr const char* kHrtimerTriggerConfigfsPath = "/config/iio/triggers/hrtimer";
static constexpr int32_t kDefaultMaxDelayUs = 10 * 1000 * 1000;
static constexpr int64_t kNanosecondsPerSecond = 1000LL * 1000 * 1000;
static constexpr int32_t kBufferLength = 128;

extern "C" __attribute__((visibility("default"))) ISensorBackend* CreateSensorBackend() {
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
            sysfs_path + "/in_anglvel_mount_matrix",
            sysfs_path + "/in_magn_mount_matrix",
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

bool IioBackend::ParseMountMatrixFromString(const std::string& content, float matrix[9]) {
    auto rows = ::android::base::Split(content, ";");
    if (rows.size() != 3) {
        return false;
    }

    float parsed[9];
    for (int r = 0; r < 3; r++) {
        auto cols = ::android::base::Split(rows[r], ",");
        if (cols.size() != 3) {
            return false;
        }
        for (int c = 0; c < 3; c++) {
            std::string trimmed = ::android::base::Trim(cols[c]);
            char* end = nullptr;
            float val = std::strtof(trimmed.c_str(), &end);
            if (end == trimmed.c_str() || *end != '\0') {
                return false;
            }
            parsed[r * 3 + c] = val;
        }
    }

    std::copy(parsed, parsed + 9, matrix);
    return true;
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

std::optional<SensorType> IioBackend::MapIioTypeToSensorType(const std::string& iio_name) {
    if (iio_name.find("accel") != std::string::npos) return SensorType::ACCELEROMETER;
    if (iio_name.find("gyro") != std::string::npos ||
        iio_name.find("anglvel") != std::string::npos)
        return SensorType::GYROSCOPE;
    if (iio_name.find("magn") != std::string::npos)
        return SensorType::MAGNETIC_FIELD;
    if (iio_name.find("light") != std::string::npos ||
        iio_name.find("illuminance") != std::string::npos ||
        iio_name.find("intensity") != std::string::npos)
        return SensorType::LIGHT;
    if (iio_name.find("proximity") != std::string::npos)
        return SensorType::PROXIMITY;
    if (iio_name.find("temp") != std::string::npos)
        return SensorType::AMBIENT_TEMPERATURE;
    if (iio_name.find("pressure") != std::string::npos ||
        iio_name.find("baro") != std::string::npos)
        return SensorType::PRESSURE;
    if (iio_name.find("humidity") != std::string::npos)
        return SensorType::RELATIVE_HUMIDITY;
    return std::nullopt;
}

std::optional<SensorType> IioBackend::DetectTypeFromScanElements(const std::string& sysfs_path) {
    std::error_code ec;
    std::filesystem::path scan_dir = std::filesystem::path(sysfs_path) / "scan_elements";
    std::filesystem::directory_iterator scan_it(scan_dir, ec);
    if (ec) {
        return std::nullopt;
    }

    std::optional<SensorType> sensor_type;
    for (const auto& scan_entry : scan_it) {
        std::string fname = scan_entry.path().filename().string();
        if (fname.size() > 3 && fname.substr(fname.size() - 3) == "_en") {
            std::string prefix = fname.substr(0, fname.size() - 3);
            if (prefix.find("in_") == 0) {
                prefix = prefix.substr(3);
            }
            auto pos = prefix.find_last_of('_');
            if (pos != std::string::npos) {
                std::string base = prefix.substr(0, pos);
                sensor_type = MapIioTypeToSensorType(base);
                if (sensor_type.has_value()) break;
            }
            sensor_type = MapIioTypeToSensorType(prefix);
            if (sensor_type.has_value()) break;
        }
    }
    return sensor_type;
}

std::optional<SensorType> IioBackend::DetectTypeFromSysfsAttributes(const std::string& sysfs_path) {
    static const struct {
        const char* prefix;
        SensorType type;
    } kAttributeMap[] = {
            {"in_accel_", SensorType::ACCELEROMETER},
            {"in_anglvel_", SensorType::GYROSCOPE},
            {"in_magn_", SensorType::MAGNETIC_FIELD},
            {"in_illuminance", SensorType::LIGHT},
            {"in_intensity", SensorType::LIGHT},
            {"in_proximity", SensorType::PROXIMITY},
            {"in_temp_", SensorType::AMBIENT_TEMPERATURE},
            {"in_pressure", SensorType::PRESSURE},
            {"in_humidityrelative", SensorType::RELATIVE_HUMIDITY},
    };

    std::error_code ec;
    for (const auto& entry : kAttributeMap) {
        std::string raw_path = sysfs_path + "/" + entry.prefix + "_raw";
        std::string input_path = sysfs_path + "/" + entry.prefix + "_input";
        if (std::filesystem::exists(raw_path, ec) || std::filesystem::exists(input_path, ec)) {
            return entry.type;
        }
    }

    std::filesystem::directory_iterator dp(sysfs_path, ec);
    if (ec) {
        return std::nullopt;
    }

    std::optional<SensorType> result;
    for (const auto& dir_entry : dp) {
        std::string fname = dir_entry.path().filename().string();
        if (fname.find("in_") != 0) {
            continue;
        }
        if (fname.find("_raw") == std::string::npos &&
            fname.find("_input") == std::string::npos) {
            continue;
        }

        std::string attr = fname.substr(3);
        auto pos = attr.find_last_of('_');
        if (pos != std::string::npos) {
            attr = attr.substr(0, pos);
        }
        pos = attr.find_last_of('_');
        if (pos != std::string::npos) {
            attr = attr.substr(0, pos);
        }

        result = MapIioTypeToSensorType(attr);
        if (result.has_value()) break;
    }
    return result;
}

std::string IioBackend::ParseVendorFromCompatible(const std::string& of_compatible) {
    if (of_compatible.empty()) {
        return "";
    }
    size_t comma_pos = of_compatible.find(',');
    if (comma_pos == std::string::npos || comma_pos == 0) {
        return "";
    }
    std::string vendor = of_compatible.substr(0, comma_pos);
    vendor[0] = std::toupper(static_cast<unsigned char>(vendor[0]));
    return vendor;
}

bool IioBackend::IsVec3Type(SensorType type) {
    return type == SensorType::ACCELEROMETER || type == SensorType::GYROSCOPE ||
           type == SensorType::MAGNETIC_FIELD;
}

std::vector<float> IioBackend::ReadAvailableFrequencies(const std::string& sysfs_path) {
    std::vector<float> frequencies;

    std::string freq_path = sysfs_path + "/sampling_frequency_available";
    std::string content = ReadSysfsString(freq_path, "");

    if (content.empty()) {
        return frequencies;
    }

    std::istringstream iss(content);
    std::string token;
    while (iss >> token) {
        char* end = nullptr;
        float freq = std::strtof(token.c_str(), &end);
        if (end != token.c_str() && *end == '\0') {
            frequencies.push_back(freq);
        }
    }

    std::sort(frequencies.begin(), frequencies.end());
    return frequencies;
}

void IioBackend::DeriveSensorInfoFromSysfs(IioSensorData* sensor) {
    if (!sensor || sensor->channels.empty()) {
        return;
    }

    const auto& channel = sensor->channels[0];
    float scale = channel.scale;
    uint8_t realbits = channel.realbits;
    bool is_signed = (channel.sign == 's');

    int32_t max_raw = is_signed ? ((1 << (realbits - 1)) - 1) : ((1 << realbits) - 1);
    float raw_max_range = static_cast<float>(max_raw) * scale;
    float resolution = scale;

    switch (sensor->type) {
        case SensorType::ACCELEROMETER:
            // IIO scale already converts raw to m/s²
            sensor->sensor_info.maxRange = raw_max_range;
            sensor->sensor_info.resolution = resolution;
            sensor->sensor_info.minDelayUs = 2500;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
            break;

        case SensorType::GYROSCOPE:
            sensor->sensor_info.maxRange = raw_max_range * static_cast<float>(M_PI) / 180.0f;
            sensor->sensor_info.resolution = resolution * static_cast<float>(M_PI) / 180.0f;
            sensor->sensor_info.minDelayUs = 2500;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
            break;

        case SensorType::MAGNETIC_FIELD:
            sensor->sensor_info.maxRange = raw_max_range;
            sensor->sensor_info.resolution = resolution;
            sensor->sensor_info.minDelayUs = 10000;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags =
                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
            break;

        case SensorType::LIGHT:
        case SensorType::PROXIMITY:
        case SensorType::AMBIENT_TEMPERATURE:
        case SensorType::RELATIVE_HUMIDITY:
        case SensorType::PRESSURE:
            sensor->sensor_info.maxRange = raw_max_range;
            sensor->sensor_info.resolution = resolution;
            sensor->sensor_info.minDelayUs = 0;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            if (sensor->type == SensorType::PROXIMITY) {
                sensor->sensor_info.flags =
                        static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE |
                                             SensorInfo::SENSOR_FLAG_BITS_WAKE_UP);
            } else {
                sensor->sensor_info.flags =
                        static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
            }
            break;

        default:
            sensor->sensor_info.maxRange = raw_max_range;
            sensor->sensor_info.resolution = resolution;
            sensor->sensor_info.minDelayUs = 0;
            sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
            sensor->sensor_info.flags = 0;
            break;
    }

    auto frequencies = ReadAvailableFrequencies(sensor->sysfs_path);
    if (!frequencies.empty()) {
        float max_freq = frequencies.back();
        float min_freq = frequencies.front();

        if (max_freq > 0) {
            int32_t computed_min_delay = static_cast<int32_t>(1000000.0f / max_freq);
            if (computed_min_delay > 0) {
                sensor->sensor_info.minDelayUs = computed_min_delay;
            }
        }

        if (min_freq > 0) {
            int32_t computed_max_delay = static_cast<int32_t>(1000000.0f / min_freq);
            if (computed_max_delay > 0 && computed_max_delay < kDefaultMaxDelayUs) {
                sensor->sensor_info.maxDelayUs = computed_max_delay;
            }
        }
    }
}

void IioBackend::ApplySensorInfoOverrides(IioSensorData* sensor) {
    if (!sensor) {
        return;
    }

    std::string device_key = sensor->device_name;
    for (char& c : device_key) {
        if (c == '-' || c == ' ' || c == '/') {
            c = '_';
        }
    }

    std::string prop_prefix = "vendor.sensors.iio." + device_key + ".";

    std::string vendor_override = ::android::base::GetProperty(prop_prefix + "vendor", "");
    if (!vendor_override.empty()) {
        sensor->sensor_info.vendor = vendor_override;
    }

    float power = 0.001f;
    std::string power_str = ::android::base::GetProperty(prop_prefix + "power", "");
    if (!power_str.empty()) {
        char* end = nullptr;
        float parsed = std::strtof(power_str.c_str(), &end);
        if (end != power_str.c_str() && *end == '\0' && parsed >= 0.0f) {
            power = parsed;
        }
    }
    sensor->sensor_info.power = power;

    std::string max_range_str = ::android::base::GetProperty(prop_prefix + "max_range", "");
    if (!max_range_str.empty()) {
        char* end = nullptr;
        float parsed = std::strtof(max_range_str.c_str(), &end);
        if (end != max_range_str.c_str() && *end == '\0' && parsed > 0.0f) {
            sensor->sensor_info.maxRange = parsed;
        }
    }

    std::string resolution_str = ::android::base::GetProperty(prop_prefix + "resolution", "");
    if (!resolution_str.empty()) {
        char* end = nullptr;
        float parsed = std::strtof(resolution_str.c_str(), &end);
        if (end != resolution_str.c_str() && *end == '\0' && parsed > 0.0f) {
            sensor->sensor_info.resolution = parsed;
        }
    }

    std::string mount_matrix_str = ::android::base::GetProperty(prop_prefix + "mount_matrix", "");
    if (!mount_matrix_str.empty()) {
        if (ParseMountMatrixFromString(mount_matrix_str, sensor->mount_matrix)) {
            LOG(INFO) << "Mount matrix overridden for " << sensor->device_name << ": "
                      << mount_matrix_str;
        } else {
            LOG(WARNING) << "Invalid mount matrix override for " << sensor->device_name
                         << ": " << mount_matrix_str;
        }
    }
}

void IioBackend::DiscoverDevices() {
    std::error_code ec;
    std::filesystem::directory_iterator dir_it(kIioBasePath, ec);
    if (ec) {
        LOG(WARNING) << "Cannot open " << kIioBasePath << ": " << ec.message();
        return;
    }

    for (const auto& entry : dir_it) {
        std::string name = entry.path().filename().string();
        if (name.find("iio:device") == std::string::npos) {
            continue;
        }

        int dev_num = -1;
        if (sscanf(name.c_str(), "iio:device%d", &dev_num) != 1) {
            continue;
        }

        DiscoverSensors(dev_num, entry.path().string());
    }
}

void IioBackend::DiscoverSensors(int dev_num, const std::string& sysfs_path) {
    std::string device_name = ReadSysfsString(sysfs_path + "/name", "unknown");
    std::string of_name = ReadSysfsString(sysfs_path + "/of_node/name", "");
    std::string of_compatible = ReadSysfsString(sysfs_path + "/of_node/compatible", "");

    LOG(INFO) << "IIO device " << dev_num << ": name='" << device_name
              << "' of_name='" << of_name << "' compatible='" << of_compatible
              << "' at " << sysfs_path;

    std::optional<SensorType> sensor_type = MapIioTypeToSensorType(device_name);

    if (!sensor_type.has_value() && !of_compatible.empty()) {
        sensor_type = MapIioTypeToSensorType(of_compatible);
    }

    if (!sensor_type.has_value() && !of_name.empty()) {
        sensor_type = MapIioTypeToSensorType(of_name);
    }

    if (!sensor_type.has_value()) {
        sensor_type = DetectTypeFromScanElements(sysfs_path);
    }

    if (!sensor_type.has_value()) {
        sensor_type = DetectTypeFromSysfsAttributes(sysfs_path);
    }

    if (!sensor_type.has_value()) {
        LOG(DEBUG) << "IIO device " << dev_num << " has no recognized sensor type, skipping";
        return;
    }

    auto sensor = std::make_unique<IioSensorData>();
    sensor->handle = next_handle_++;
    sensor->sysfs_path = sysfs_path;
    sensor->device_name = device_name;
    sensor->type = *sensor_type;
    sensor->is_poll_mode = true;
    sensor->enabled = false;
    sensor->sampling_period_ns = 200 * 1000 * 1000;
    sensor->stop_thread = false;
    sensor->dev_num = dev_num;

    ParseMountMatrix(sysfs_path, sensor->mount_matrix);

    bool has_scan_elements = false;
    std::filesystem::path scan_dir = std::filesystem::path(sysfs_path) / "scan_elements";
    std::error_code ec;
    std::filesystem::directory_iterator scan_it(scan_dir, ec);
    if (!ec) {
        for (const auto& scan_entry : scan_it) {
            std::string fname = scan_entry.path().filename().string();
            if (fname.size() > 3 && fname.substr(fname.size() - 3) == "_en") {
                has_scan_elements = true;
                std::string en_content = ReadSysfsString(scan_dir.string() + "/" + fname, "0");
                if (en_content != "1") {
                    continue;
                }

                std::string chan_prefix = fname.substr(0, fname.size() - 3);

                IioChannelInfo channel;
                channel.name = chan_prefix;

                std::string type_content =
                        ReadSysfsString(scan_dir.string() + "/" + chan_prefix + "_type", "");
                if (type_content.empty()) {
                    continue;
                }

                if (!ParseChannelType(type_content, channel)) {
                    continue;
                }

                channel.index = ReadSysfsInt(scan_dir.string() + "/" + chan_prefix + "_index", -1);
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
                if (!std::filesystem::exists(raw_path, ec)) {
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
                if (std::filesystem::exists(test, ec)) {
                    found_path = test;
                    break;
                }
            }

            if (found_path.empty()) {
                for (const auto& suffix : suffixes) {
                    std::string test = sysfs_path + "/in_" + type_prefix + "0" + suffix;
                    if (std::filesystem::exists(test, ec)) {
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
        if (std::filesystem::exists(dev_path, ec)) {
            sensor->is_poll_mode = false;
        }
    }

    std::sort(sensor->channels.begin(), sensor->channels.end(),
              [](const IioChannelInfo& a, const IioChannelInfo& b) {
                  return a.index < b.index;
              });

    sensor->sensor_info.sensorHandle = sensor->handle;
    sensor->sensor_info.name = device_name;
    std::string parsed_vendor = ParseVendorFromCompatible(of_compatible);
    sensor->sensor_info.vendor = parsed_vendor.empty() ? "Linux IIO" : parsed_vendor;
    sensor->sensor_info.version = 1;
    sensor->sensor_info.type = sensor->type;
    sensor->sensor_info.typeAsString = "";
    sensor->sensor_info.fifoReservedEventCount = 0;
    sensor->sensor_info.fifoMaxEventCount = 0;
    sensor->sensor_info.requiredPermission = "";

    DeriveSensorInfoFromSysfs(sensor.get());
    ApplySensorInfoOverrides(sensor.get());

    int32_t handle = sensor->handle;
    sensors_[handle] = std::move(sensor);
    LOG(INFO) << "IIO sensor discovered: " << device_name << " (handle=" << handle
              << ", type=" << static_cast<int32_t>(*sensor_type) << ", poll_mode="
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
        if (!sensor->is_poll_mode && !sensor->trigger_name.empty()) {
            TeardownHrtimerTrigger(sensor.get());
        }
        CloseBufferFd(sensor.get());
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

    if (!sensor->is_poll_mode) {
        BufferSensorThread(sensor);
        return;
    }

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

        std::vector<Event> events = ReadPollSensorData(sensor);

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

        // IIO scale already converts raw to m/s²

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

bool IioBackend::SetupHrtimerTrigger(IioSensorData* sensor) {
    std::error_code ec;
    if (!std::filesystem::exists(kHrtimerTriggerConfigfsPath, ec)) {
        LOG(WARNING) << "hrtimer trigger configfs not available at "
                     << kHrtimerTriggerConfigfsPath;
        return false;
    }

    sensor->trigger_name = "mainline_sensors_" + std::to_string(sensor->dev_num);
    std::string trigger_path =
            std::string(kHrtimerTriggerConfigfsPath) + "/" + sensor->trigger_name;

    if (!std::filesystem::exists(trigger_path, ec)) {
        if (!std::filesystem::create_directory(trigger_path, ec)) {
            LOG(WARNING) << "Failed to create hrtimer trigger " << trigger_path
                         << ": " << ec.message();
            return false;
        }
    }

    if (sensor->sampling_period_ns > 0) {
        int32_t freq = static_cast<int32_t>(
                static_cast<float>(kNanosecondsPerSecond) /
                static_cast<float>(sensor->sampling_period_ns));
        if (freq < 1) freq = 1;
        std::string trig_freq_path =
                    std::string(kIioBasePath) + "/" + sensor->trigger_name + "/sampling_frequency";
        if (!WriteSysfsInt(trig_freq_path, freq)) {
            LOG(WARNING) << "Failed to set trigger sampling frequency at " << trig_freq_path;
        }
    }

    std::string current_trigger_path = sensor->sysfs_path + "/trigger/current_trigger";
    if (!::android::base::WriteStringToFile(sensor->trigger_name, current_trigger_path)) {
        LOG(WARNING) << "Failed to assign trigger " << sensor->trigger_name
                     << " to device " << sensor->dev_num;
        return false;
    }

    LOG(INFO) << "hrtimer trigger " << sensor->trigger_name
              << " set up for device " << sensor->dev_num;
    return true;
}

void IioBackend::TeardownHrtimerTrigger(IioSensorData* sensor) {
    if (sensor->trigger_name.empty()) {
        return;
    }

    std::string current_trigger_path = sensor->sysfs_path + "/trigger/current_trigger";
    ::android::base::WriteStringToFile("", current_trigger_path);

    std::error_code ec;
    std::string trigger_path =
            std::string(kHrtimerTriggerConfigfsPath) + "/" + sensor->trigger_name;
    std::filesystem::remove(trigger_path, ec);

    LOG(INFO) << "hrtimer trigger " << sensor->trigger_name << " torn down";
    sensor->trigger_name.clear();
}

bool IioBackend::OpenBufferFd(IioSensorData* sensor) {
    sensor->scan_size = 0;
    for (const auto& channel : sensor->channels) {
        int32_t end = channel.location + (channel.storagebits + 7) / 8;
        if (end > sensor->scan_size) {
            sensor->scan_size = end;
        }
    }

    if (sensor->scan_size <= 0) {
        LOG(WARNING) << "Invalid scan size for sensor " << sensor->dev_num;
        return false;
    }

    std::string dev_path = "/dev/iio:device" + std::to_string(sensor->dev_num);
    sensor->buffer_fd.reset(open(dev_path.c_str(), O_RDONLY));
    if (!sensor->buffer_fd.ok()) {
        LOG(WARNING) << "Failed to open " << dev_path << ": " << strerror(errno);
        return false;
    }

    if (pipe2(sensor->signal_pipe_fd, O_CLOEXEC | O_NONBLOCK) != 0) {
        LOG(WARNING) << "Failed to create signal pipe: " << strerror(errno);
        sensor->buffer_fd.reset();
        return false;
    }

    return true;
}

void IioBackend::CloseBufferFd(IioSensorData* sensor) {
    sensor->buffer_fd.reset();
    if (sensor->signal_pipe_fd[0] >= 0) {
        close(sensor->signal_pipe_fd[0]);
        sensor->signal_pipe_fd[0] = -1;
    }
    if (sensor->signal_pipe_fd[1] >= 0) {
        close(sensor->signal_pipe_fd[1]);
        sensor->signal_pipe_fd[1] = -1;
    }
}

bool IioBackend::EnableRingBuffer(IioSensorData* sensor, bool enable) {
    std::string buffer_path = sensor->sysfs_path + "/buffer/enable";
    std::string length_path = sensor->sysfs_path + "/buffer/length";

    if (enable) {
        if (!SetupHrtimerTrigger(sensor)) {
            LOG(WARNING) << "Trigger setup failed for device " << sensor->dev_num
                         << ", falling back to sysfs poll mode";
            return false;
        }

        std::string scan_dir = sensor->sysfs_path + "/scan_elements";
        std::error_code ec;
        std::filesystem::directory_iterator scan_it(scan_dir, ec);
        if (!ec) {
            for (const auto& scan_entry : scan_it) {
                std::string fname = scan_entry.path().filename().string();
                if (fname.size() > 3 && fname.substr(fname.size() - 3) == "_en") {
                    ::android::base::WriteStringToFile("1", scan_dir + "/" + fname);
                }
            }
        }

        WriteSysfsInt(length_path, kBufferLength);
        WriteSysfsInt(buffer_path, 1);

        if (!OpenBufferFd(sensor)) {
            LOG(WARNING) << "Failed to open buffer fd for device " << sensor->dev_num
                         << ", falling back to sysfs poll mode";
            WriteSysfsInt(buffer_path, 0);
            TeardownHrtimerTrigger(sensor);
            return false;
        }

        return true;
    } else {
        if (sensor->buffer_fd.ok()) {
            char shutdown_byte = 1;
            ssize_t ret = write(sensor->signal_pipe_fd[1], &shutdown_byte, 1);
            if (ret < 0) {
                LOG(WARNING) << "Failed to write shutdown signal: " << strerror(errno);
            }
        }

        WriteSysfsInt(buffer_path, 0);
        CloseBufferFd(sensor);
        TeardownHrtimerTrigger(sensor);
        return true;
    }
}

std::vector<Event> IioBackend::ParseBufferSamples(IioSensorData* sensor,
                                                   const uint8_t* data,
                                                   size_t num_samples) {
    std::vector<Event> events;
    if (num_samples == 0 || sensor->scan_size <= 0) {
        return events;
    }

    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    int64_t timestamp = ts.tv_sec * kNanosecondsPerSecond + ts.tv_nsec;

    if (IsVec3Type(sensor->type) && sensor->channels.size() >= 3) {
        const uint8_t* last_sample = data + (num_samples - 1) * sensor->scan_size;
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

        // IIO scale already converts raw to m/s²

        Event event;
        event.sensorHandle = sensor->handle;
        event.sensorType = sensor->type;
        event.timestamp = timestamp;
        EventPayload::Vec3 vec3 = BuildVec3Value(
                {corrected[0], corrected[1], corrected[2]});
        event.payload.set<EventPayload::Tag::vec3>(vec3);
        events.push_back(event);
    } else if (!sensor->channels.empty()) {
        const uint8_t* last_sample = data + (num_samples - 1) * sensor->scan_size;
        const auto& channel = sensor->channels[0];
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

        float value = (static_cast<float>(raw_value) + channel.offset) * channel.scale;

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

void IioBackend::BufferSensorThread(IioSensorData* sensor) {
    LOG(DEBUG) << "Buffer thread started for sensor " << sensor->handle;

    struct pollfd fds[2];
    fds[0].fd = sensor->buffer_fd.get();
    fds[0].events = POLLIN;
    fds[1].fd = sensor->signal_pipe_fd[0];
    fds[1].events = POLLIN;

    std::vector<uint8_t> raw_buf(sensor->scan_size * kBufferLength);

    while (sensor->enabled.load() && !sensor->stop_thread.load() &&
           operation_mode_ != OperationMode::DATA_INJECTION) {
        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG(WARNING) << "poll() failed for sensor " << sensor->handle
                         << ": " << strerror(errno);
            break;
        }
        if (ret == 0) continue;

        if (fds[1].revents & (POLLIN | POLLHUP)) {
            break;
        }

        if (fds[0].revents & POLLIN) {
            ssize_t bytes_read = read(sensor->buffer_fd.get(), raw_buf.data(), raw_buf.size());
            if (bytes_read < static_cast<ssize_t>(sensor->scan_size)) {
                continue;
            }

            size_t num_samples = static_cast<size_t>(bytes_read) / sensor->scan_size;
            std::vector<Event> events = ParseBufferSamples(sensor, raw_buf.data(), num_samples);

            if (!events.empty() && post_events_callback_) {
                bool wakeup =
                        (sensor->sensor_info.flags &
                         static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP)) != 0;
                post_events_callback_(events, wakeup);
            }
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG(WARNING) << "Buffer fd error for sensor " << sensor->handle;
            break;
        }
    }

    LOG(DEBUG) << "Buffer thread stopped for sensor " << sensor->handle;
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

    if (enabled) {
        sensor->stop_thread = false;
        sensor->enabled = true;

        if (!sensor->is_poll_mode) {
            if (!EnableRingBuffer(sensor.get(), true)) {
                sensor->is_poll_mode = true;
            }
        }

        sensor->poll_thread = std::thread(&IioBackend::PollSensorThread, this, sensor.get());
    } else {
        sensor->enabled = false;

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

    if (sampling_period_ns > 0) {
        int32_t freq = static_cast<int32_t>(
                static_cast<float>(kNanosecondsPerSecond) /
                static_cast<float>(sampling_period_ns));
        if (freq < 1) freq = 1;

        std::string freq_path = sensor->sysfs_path + "/sampling_frequency";
        WriteSysfsInt(freq_path, freq);

        if (!sensor->is_poll_mode && !sensor->trigger_name.empty()) {
            std::string trig_freq_path =
                std::string(kIioBasePath) + "/" + sensor->trigger_name + "/sampling_frequency";
            WriteSysfsInt(trig_freq_path, freq);
        }
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
