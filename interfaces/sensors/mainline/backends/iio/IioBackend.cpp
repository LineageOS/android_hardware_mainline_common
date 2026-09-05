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

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>

namespace aidl::android::hardware::sensors::mainline {
namespace {

constexpr char kIioPath[] = "/sys/bus/iio/devices";
constexpr char kHrtimerPath[] = "/config/iio/triggers/hrtimer";
constexpr int64_t kNsPerSecond = 1000000000LL;
constexpr int kBufferLength = 128;
constexpr int64_t kDefaultPeriodNs = 200000000LL;
constexpr int32_t kDefaultMaxDelayUs = 10000000;

int Axis(const std::string& name) {
    if (name.size() < 2 || name[name.size() - 2] != '_') return 3;
    if (name.back() == 'x') return 0;
    if (name.back() == 'y') return 1;
    if (name.back() == 'z') return 2;
    return 3;
}

bool OrderVector(std::vector<IioChannelInfo>* channels) {
    std::stable_sort(channels->begin(), channels->end(), [](const auto& left, const auto& right) {
        const int left_axis = Axis(left.name);
        const int right_axis = Axis(right.name);
        return left_axis != right_axis ? left_axis < right_axis : left.index < right.index;
    });
    return channels->size() >= 3 && Axis((*channels)[0].name) == 0 &&
           Axis((*channels)[1].name) == 1 && Axis((*channels)[2].name) == 2;
}

std::vector<double> ParseFrequencies(std::string text) {
    std::replace(text.begin(), text.end(), '[', ' ');
    std::replace(text.begin(), text.end(), ']', ' ');
    std::istringstream stream(text);
    std::vector<double> frequencies;
    double frequency;
    while (stream >> frequency) {
        if (std::isfinite(frequency) && frequency > 0.0) frequencies.push_back(frequency);
    }
    return frequencies;
}

}  // namespace

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

std::string IioBackend::ReadString(const std::string& path, const std::string& fallback) const {
    std::string value;
    return ::android::base::ReadFileToString(path, &value) ? ::android::base::Trim(value)
                                                           : fallback;
}

double IioBackend::ReadDouble(const std::string& path, double fallback) const {
    const std::string text = ReadString(path);
    if (text.empty()) return fallback;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' ? value : fallback;
}

int IioBackend::ReadInt(const std::string& path, int fallback) const {
    const std::string text = ReadString(path);
    if (text.empty()) return fallback;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && *end == '\0' ? static_cast<int>(value) : fallback;
}

bool IioBackend::WriteString(const std::string& path, const std::string& value) const {
    return ::android::base::WriteStringToFile(value, path);
}

bool IioBackend::Exists(const std::string& path) const {
    std::error_code error;
    return std::filesystem::exists(path, error);
}

double IioBackend::ReadSharedAttribute(const std::string& path, const std::string& channel,
                                       const std::string& suffix, double fallback) const {
    const std::string separate = path + "/" + channel + "_" + suffix;
    if (Exists(separate)) return ReadDouble(separate, fallback);
    const size_t axis = channel.find_last_of('_');
    if (axis != std::string::npos && axis + 2 == channel.size() &&
        (channel.back() == 'x' || channel.back() == 'y' || channel.back() == 'z')) {
        const std::string shared = path + "/" + channel.substr(0, axis) + "_" + suffix;
        if (Exists(shared)) return ReadDouble(shared, fallback);
    }
    return fallback;
}

std::optional<SensorType> IioBackend::MapIioType(const std::string& input) const {
    std::string name = input;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (name.find("accel") != std::string::npos) return SensorType::ACCELEROMETER;
    if (name.find("anglvel") != std::string::npos || name.find("gyro") != std::string::npos)
        return SensorType::GYROSCOPE;
    if (name.find("magn") != std::string::npos || name.find("compass") != std::string::npos)
        return SensorType::MAGNETIC_FIELD;
    if (name.find("proximity") != std::string::npos || name.find("prox") != std::string::npos)
        return SensorType::PROXIMITY;
    if (name.find("illuminance") != std::string::npos ||
        name.find("intensity") != std::string::npos || name.find("light") != std::string::npos)
        return SensorType::LIGHT;
    if (name.find("pressure") != std::string::npos || name.find("baro") != std::string::npos)
        return SensorType::PRESSURE;
    if (name.find("humidity") != std::string::npos) return SensorType::RELATIVE_HUMIDITY;
    if (name.find("temp") != std::string::npos) return SensorType::AMBIENT_TEMPERATURE;
    return std::nullopt;
}

std::optional<SensorType> IioBackend::ClassifyChannel(const std::string& name) const {
    if (name.find("timestamp") != std::string::npos) return std::nullopt;
    return MapIioType(name.starts_with("in_") ? name.substr(3) : name);
}

std::string IioBackend::TypePrefix(SensorType type) const {
    switch (type) {
        case SensorType::ACCELEROMETER:
            return "accel";
        case SensorType::GYROSCOPE:
            return "anglvel";
        case SensorType::MAGNETIC_FIELD:
            return "magn";
        case SensorType::LIGHT:
            return "illuminance";
        case SensorType::PROXIMITY:
            return "proximity";
        case SensorType::PRESSURE:
            return "pressure";
        case SensorType::RELATIVE_HUMIDITY:
            return "humidityrelative";
        case SensorType::AMBIENT_TEMPERATURE:
            return "temp";
        default:
            return "";
    }
}

iio::Unit IioBackend::UnitForType(SensorType type) const {
    switch (type) {
        case SensorType::ACCELEROMETER:
            return iio::Unit::kAcceleration;
        case SensorType::GYROSCOPE:
            return iio::Unit::kAngularVelocity;
        case SensorType::MAGNETIC_FIELD:
            return iio::Unit::kMagneticFieldGauss;
        case SensorType::PRESSURE:
            return iio::Unit::kPressureKilopascal;
        case SensorType::RELATIVE_HUMIDITY:
            return iio::Unit::kRelativeHumidityMilliPercent;
        case SensorType::AMBIENT_TEMPERATURE:
            return iio::Unit::kTemperatureMilliCelsius;
        case SensorType::PROXIMITY:
            return iio::Unit::kProximityMeters;
        default:
            return iio::Unit::kUnchanged;
    }
}

bool IioBackend::IsVector(SensorType type) const {
    return type == SensorType::ACCELEROMETER || type == SensorType::GYROSCOPE ||
           type == SensorType::MAGNETIC_FIELD;
}

bool IioBackend::IsOnChange(SensorType type) const {
    return type == SensorType::LIGHT || type == SensorType::PROXIMITY ||
           type == SensorType::AMBIENT_TEMPERATURE || type == SensorType::RELATIVE_HUMIDITY;
}

int64_t IioBackend::BoottimeNs() const {
    timespec time{};
    clock_gettime(CLOCK_BOOTTIME, &time);
    return time.tv_sec * kNsPerSecond + time.tv_nsec;
}

std::vector<IioChannelInfo> IioBackend::ReadScanChannels(const std::string& path) {
    std::vector<IioChannelInfo> result;
    const std::string scan_path = path + "/scan_elements";
    std::error_code error;
    std::filesystem::directory_iterator entries(scan_path, error);
    if (error) return result;
    for (const auto& entry : entries) {
        const std::string file = entry.path().filename().string();
        if (!file.ends_with("_en")) continue;
        const std::string name = file.substr(0, file.size() - 3);
        IioChannelInfo channel;
        channel.name = name;
        channel.index = ReadInt(scan_path + "/" + name + "_index", -1);
        if (channel.index < 0 ||
            !iio::ParseScanType(ReadString(scan_path + "/" + name + "_type"), &channel.scan_type))
            continue;
        channel.scale = ReadSharedAttribute(path, name, "scale", 1.0);
        channel.offset = ReadSharedAttribute(path, name, "offset", 0.0);
        result.push_back(channel);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.index != b.index ? a.index < b.index : a.name < b.name;
    });
    return result;
}

std::set<SensorType> IioBackend::DetectTypes(const std::string& path, const std::string& name,
                                             const std::string& compatible,
                                             const std::string& of_name,
                                             const std::vector<IioChannelInfo>& channels) {
    std::set<SensorType> types;
    for (const auto& channel : channels) {
        if (auto type = ClassifyChannel(channel.name)) types.insert(*type);
    }
    std::error_code error;
    std::filesystem::directory_iterator entries(path, error);
    if (!error) {
        for (const auto& entry : entries) {
            const std::string file = entry.path().filename().string();
            if (file.starts_with("in_") && (file.ends_with("_raw") || file.ends_with("_input"))) {
                if (auto type = MapIioType(file)) types.insert(*type);
            }
        }
    }
    if (types.empty()) {
        for (const auto& candidate : {name, compatible, of_name}) {
            if (auto type = MapIioType(candidate)) {
                types.insert(*type);
                break;
            }
        }
    }
    return types;
}

std::vector<IioDirectSource> IioBackend::FindDirectSources(const std::string& path,
                                                           SensorType type) {
    std::vector<IioDirectSource> result;
    std::vector<std::string> prefixes = {TypePrefix(type)};
    if (type == SensorType::GYROSCOPE) prefixes.push_back("gyro");
    if (type == SensorType::LIGHT) prefixes.insert(prefixes.end(), {"intensity", "light"});
    if (type == SensorType::PROXIMITY) prefixes.push_back("prox");
    if (type == SensorType::RELATIVE_HUMIDITY) prefixes.push_back("humidity");

    const auto find_source =
            [&](const std::vector<std::string>& axes) -> std::optional<IioDirectSource> {
        for (const auto& axis : axes) {
            for (const auto& prefix : prefixes) {
                const std::string base = "in_" + prefix + axis;
                for (const auto& suffix : {std::string("_input"), std::string("_raw")}) {
                    const std::string source = path + "/" + base + suffix;
                    if (!Exists(source)) continue;
                    return IioDirectSource{
                            .path = source,
                            .is_input = suffix == "_input",
                            .scale = ReadSharedAttribute(path, base, "scale", 1.0),
                            .offset = ReadSharedAttribute(path, base, "offset", 0.0)};
                }
            }
        }
        return std::nullopt;
    };

    if (!IsVector(type)) {
        if (auto source = find_source({"", "0"})) result.push_back(*source);
        return result;
    }
    for (const auto& axis : {std::string("_x"), std::string("_y"), std::string("_z")}) {
        bool found = false;
        if (auto source = find_source({axis})) {
            result.push_back(*source);
            found = true;
        }
        if (!found) return {};
    }
    return result;
}

bool IioBackend::ParseMountMatrixString(const std::string& text, float matrix[9]) {
    const auto rows = ::android::base::Split(text, ";");
    if (rows.size() != 3) return false;
    float parsed[9];
    for (size_t row = 0; row < 3; ++row) {
        const auto columns = ::android::base::Split(rows[row], ",");
        if (columns.size() != 3) return false;
        for (size_t column = 0; column < 3; ++column) {
            const std::string value = ::android::base::Trim(columns[column]);
            char* end = nullptr;
            parsed[row * 3 + column] = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed[row * 3 + column]))
                return false;
        }
    }
    if (std::none_of(parsed, parsed + 9, [](float value) { return value != 0.0f; })) return false;
    std::copy(parsed, parsed + 9, matrix);
    return true;
}

void IioBackend::ParseMountMatrix(const std::string& path, SensorType type, float matrix[9]) {
    std::fill(matrix, matrix + 9, 0.0f);
    matrix[0] = matrix[4] = matrix[8] = 1.0f;
    const std::vector<std::string> candidates = {path + "/in_" + TypePrefix(type) + "_mount_matrix",
                                                 path + "/mount_matrix", path + "/in_mount_matrix"};
    for (const auto& candidate : candidates) {
        if (ParseMountMatrixString(ReadString(candidate), matrix)) return;
    }
}

void IioBackend::InitializeSensorInfo(IioSensorData* sensor, const std::string& compatible) {
    sensor->sensor_info.sensorHandle = sensor->handle;
    sensor->sensor_info.name =
            sensor->device_name + " " + TypePrefix(sensor->type) + " [" +
            (sensor->label.empty() ? "iio:device" + std::to_string(sensor->dev_num)
                                   : sensor->label) +
            "]";
    const size_t comma = compatible.find(',');
    sensor->sensor_info.vendor =
            comma == std::string::npos ? "Linux IIO" : compatible.substr(0, comma);
    if (!sensor->sensor_info.vendor.empty())
        sensor->sensor_info.vendor[0] =
                std::toupper(static_cast<unsigned char>(sensor->sensor_info.vendor[0]));
    sensor->sensor_info.version = 1;
    sensor->sensor_info.type = sensor->type;
    sensor->sensor_info.typeAsString = "";
    sensor->sensor_info.fifoReservedEventCount = 0;
    sensor->sensor_info.fifoMaxEventCount = 0;
    sensor->sensor_info.requiredPermission = "";
    DeriveSensorInfo(sensor);
    ApplyHwdb(sensor);
    ApplyOverrides(sensor);
}

void IioBackend::DeriveSensorInfo(IioSensorData* sensor) {
    double scale = 1.0;
    unsigned int bits = 16;
    bool is_signed = true;
    if (!sensor->channels.empty()) {
        scale = sensor->channels[0].scale;
        bits = sensor->channels[0].scan_type.real_bits;
        is_signed = sensor->channels[0].scan_type.is_signed;
    } else if (!sensor->direct_sources.empty() && !sensor->direct_sources[0].is_input) {
        scale = sensor->direct_sources[0].scale;
    }
    const long double raw_max =
            is_signed ? std::ldexp(1.0L, bits - 1) - 1.0L : std::ldexp(1.0L, bits) - 1.0L;
    const double unit_scale = iio::ConvertUnit(scale, UnitForType(sensor->type));
    sensor->sensor_info.maxRange = static_cast<float>(raw_max * unit_scale);
    sensor->sensor_info.resolution = static_cast<float>(std::abs(unit_scale));
    sensor->sensor_info.minDelayUs = IsOnChange(sensor->type) ? 0 : 2500;
    sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
    sensor->sensor_info.flags =
            IsOnChange(sensor->type)
                    ? static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE)
                    : static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_CONTINUOUS_MODE);
    if (sensor->type == SensorType::PROXIMITY) {
        sensor->sensor_info.flags |= static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP);
    }

    std::string available = ReadString(sensor->sysfs_path + "/sampling_frequency_available");
    if (available.empty())
        available = ReadString(sensor->sysfs_path + "/in_" + TypePrefix(sensor->type) +
                               "_sampling_frequency_available");
    const std::vector<double> frequencies = ParseFrequencies(available);
    if (!frequencies.empty()) {
        const auto [minimum, maximum] = std::minmax_element(frequencies.begin(), frequencies.end());
        if (!IsOnChange(sensor->type)) sensor->sensor_info.minDelayUs = 1000000.0 / *maximum;
        sensor->sensor_info.maxDelayUs = std::min<double>(kDefaultMaxDelayUs, 1000000.0 / *minimum);
    }
}

void IioBackend::ApplyHwdb(IioSensorData* sensor) {
    if (!sensor_hwdb_) return;
    float matrix[9];
    if (sensor->type == SensorType::ACCELEROMETER &&
        sensor_hwdb_->GetMountMatrix(sensor->parent_modalias, sensor->label, matrix)) {
        std::copy(matrix, matrix + 9, sensor->mount_matrix);
    }
    if (sensor->type == SensorType::PROXIMITY) {
        sensor->proximity_near_level =
                sensor_hwdb_->GetProximityNearLevel(sensor->parent_modalias, sensor->label, -1);
        if (sensor->proximity_near_level >= 0) {
            sensor->sensor_info.maxRange = 5.0f;
            sensor->sensor_info.resolution = 5.0f;
        }
    }
}

void IioBackend::ApplyOverrides(IioSensorData* sensor) {
    std::string key = sensor->device_name;
    for (char& character : key)
        if (character == '-' || character == ' ' || character == '/') character = '_';
    const std::string prefix = "vendor.sensors.iio." + key + ".";
    const std::string vendor = ::android::base::GetProperty(prefix + "vendor", "");
    if (!vendor.empty()) sensor->sensor_info.vendor = vendor;
    const auto read_property = [&](const std::string& name, float fallback) {
        const std::string text = ::android::base::GetProperty(prefix + name, "");
        if (text.empty()) return fallback;
        char* end = nullptr;
        const float value = std::strtof(text.c_str(), &end);
        return end != text.c_str() && *end == '\0' ? value : fallback;
    };
    const float power = read_property("power", 0.001f);
    sensor->sensor_info.power = power >= 0.0f ? power : 0.001f;
    const float max_range = read_property("max_range", -1.0f);
    const float resolution = read_property("resolution", -1.0f);
    if (max_range > 0.0f) sensor->sensor_info.maxRange = max_range;
    if (resolution > 0.0f) sensor->sensor_info.resolution = resolution;
    const std::string matrix = ::android::base::GetProperty(prefix + "mount_matrix", "");
    if (!matrix.empty() && !ParseMountMatrixString(matrix, sensor->mount_matrix)) {
        LOG(WARNING) << "Invalid mount matrix property for " << sensor->device_name;
    }
}

void IioBackend::DiscoverDevice(int dev_num, const std::string& path) {
    const std::string name = ReadString(path + "/name", "iio:device" + std::to_string(dev_num));
    const std::string compatible = ReadString(path + "/of_node/compatible");
    const std::string of_name = ReadString(path + "/of_node/name");
    const std::string label = ReadString(path + "/label");
    const std::string modalias = ReadString(path + "/../modalias");
    auto channels = ReadScanChannels(path);
    const auto types = DetectTypes(path, name, compatible, of_name, channels);
    if (types.empty()) return;

    std::shared_ptr<IioDeviceState> device;
    if (!channels.empty() && Exists("/dev/iio:device" + std::to_string(dev_num))) {
        device = std::make_shared<IioDeviceState>();
        device->dev_num = dev_num;
        device->sysfs_path = path;
        device->device_name = name;
        device->available_channels = channels;
        devices_[dev_num] = device;
    }

    for (SensorType type : types) {
        std::vector<IioChannelInfo> typed;
        for (const auto& channel : channels) {
            if (ClassifyChannel(channel.name) == type) typed.push_back(channel);
        }
        if (typed.empty() && types.size() == 1) {
            for (const auto& channel : channels) {
                if (channel.name.find("timestamp") == std::string::npos) typed.push_back(channel);
            }
        }
        if (IsVector(type) && !OrderVector(&typed)) typed.clear();
        if (!IsVector(type) && typed.size() > 1) typed.resize(1);
        auto direct = FindDirectSources(path, type);
        if (typed.empty() && direct.empty()) continue;

        auto sensor = std::make_shared<IioSensorData>();
        sensor->handle = next_handle_++;
        sensor->dev_num = dev_num;
        sensor->sysfs_path = path;
        sensor->device_name = name;
        sensor->type = type;
        sensor->channels = std::move(typed);
        sensor->direct_sources = std::move(direct);
        sensor->device = sensor->channels.empty() ? nullptr : device;
        sensor->direct_poll = sensor->device == nullptr;
        sensor->parent_modalias = modalias;
        sensor->label = label;
        ParseMountMatrix(path, type, sensor->mount_matrix);
        InitializeSensorInfo(sensor.get(), compatible);
        if (device) device->sensors.push_back(sensor.get());
        LOG(INFO) << "Discovered " << sensor->sensor_info.name << " handle=" << sensor->handle
                  << (sensor->device ? " buffered" : " direct");
        sensors_[sensor->handle] = std::move(sensor);
    }
    if (device && device->sensors.empty()) devices_.erase(dev_num);
}

void IioBackend::DiscoverDevices() {
    std::vector<std::pair<int, std::string>> devices;
    std::error_code error;
    std::filesystem::directory_iterator entries(kIioPath, error);
    if (error) return;
    for (const auto& entry : entries) {
        const std::string file = entry.path().filename().string();
        int number;
        if (sscanf(file.c_str(), "iio:device%d", &number) == 1)
            devices.emplace_back(number, entry.path().string());
    }
    std::sort(devices.begin(), devices.end());
    for (const auto& [number, path] : devices) DiscoverDevice(number, path);
}

int32_t IioBackend::Initialize(const PostEventsCallback& callback) {
    Deinitialize();
    std::lock_guard lock(mutex_);
    {
        std::lock_guard callback_lock(callback_mutex_);
        callback_ = callback;
    }
    operation_mode_ = OperationMode::NORMAL;
    sensor_hwdb_ = SensorHwdb::Create();
    DiscoverDevices();
    return 0;
}

void IioBackend::Deinitialize() {
    std::lock_guard backend_lock(mutex_);
    std::vector<IioSensorData*> sensors;
    std::vector<std::shared_ptr<IioDeviceState>> devices;
    {
        std::lock_guard callback_lock(callback_mutex_);
        callback_ = nullptr;
    }
    for (auto& [handle, sensor] : sensors_) {
        sensor->enabled = false;
        sensor->stop_thread = true;
        sensor->poll_cv.notify_all();
        sensors.push_back(sensor.get());
    }
    for (auto& [number, device] : devices_) devices.push_back(device);
    for (auto* sensor : sensors) {
        std::lock_guard lifecycle_lock(sensor->lifecycle_mutex);
        if (sensor->poll_thread.joinable()) sensor->poll_thread.join();
    }
    for (const auto& device : devices) {
        std::lock_guard config_lock(device->config_mutex);
        StopReader(device.get());
        DisableBuffer(device.get());
        ReleaseOwnedTrigger(device.get());
    }
    sensors_.clear();
    devices_.clear();
    sensor_hwdb_.reset();
    next_handle_ = 1;
}

std::vector<SensorInfo> IioBackend::GetSensorsList() {
    std::lock_guard lock(mutex_);
    std::vector<SensorInfo> result;
    for (const auto& [handle, sensor] : sensors_) result.push_back(sensor->sensor_info);
    return result;
}

void IioBackend::StopReader(IioDeviceState* device) {
    device->stop_reader = true;
    if (device->signal_pipe_fd[1] >= 0) {
        const uint8_t signal = 1;
        (void)write(device->signal_pipe_fd[1], &signal, sizeof(signal));
    }
    if (device->reader_thread.joinable()) device->reader_thread.join();
    device->reader_running = false;
    device->buffer_fd.reset();
    for (int& fd : device->signal_pipe_fd) {
        if (fd >= 0) close(fd);
        fd = -1;
    }
    device->partial_data.clear();
    std::vector<int32_t> flushes;
    {
        std::lock_guard lock(device->mutex);
        flushes.swap(device->pending_flushes);
    }
    PostFlushes(flushes);
}

void IioBackend::DisableBuffer(IioDeviceState* device) {
    WriteString(device->sysfs_path + "/buffer/enable", "0");
}

void IioBackend::ReleaseOwnedTrigger(IioDeviceState* device) {
    if (device->trigger_attached_by_hal)
        WriteString(device->sysfs_path + "/trigger/current_trigger", "\n");
    if (device->hrtimer_owned) {
        std::error_code error;
        std::filesystem::remove(std::string(kHrtimerPath) + "/" + device->trigger_name, error);
    }
    device->trigger_name.clear();
    device->trigger_attached_by_hal = false;
    device->hrtimer_trigger = false;
    device->hrtimer_owned = false;
}

bool IioBackend::ConfigureScanMask(IioDeviceState* device,
                                   const std::vector<IioSensorData*>& active) {
    std::set<std::string> required;
    for (const auto* sensor : active)
        for (const auto& channel : sensor->channels) required.insert(channel.name);
    for (const auto& channel : device->available_channels) {
        if (channel.name.find("timestamp") != std::string::npos) required.insert(channel.name);
    }
    const std::string scan_path = device->sysfs_path + "/scan_elements/";
    for (const auto& channel : device->available_channels) {
        if (!WriteString(scan_path + channel.name + "_en",
                         required.contains(channel.name) ? "1" : "0")) {
            LOG(WARNING) << "Cannot configure scan channel " << channel.name;
            return false;
        }
    }

    device->effective_channels.clear();
    std::vector<iio::ScanElement> layout;
    for (const auto& channel : device->available_channels) {
        if (ReadInt(scan_path + channel.name + "_en", 0) == 0) continue;
        device->effective_channels.push_back(channel);
        layout.push_back({.index = channel.index,
                          .type = channel.scan_type,
                          .is_timestamp = channel.name.find("timestamp") != std::string::npos});
    }
    for (const auto& name : required) {
        if (std::none_of(device->effective_channels.begin(), device->effective_channels.end(),
                         [&](const auto& channel) { return channel.name == name; }))
            return false;
    }
    auto stride = iio::ComputeScanLayout(&layout);
    if (!stride) return false;
    for (const auto& element : layout) {
        const auto channel =
                std::find_if(device->effective_channels.begin(), device->effective_channels.end(),
                             [&](const auto& item) { return item.index == element.index; });
        if (channel == device->effective_channels.end()) return false;
        channel->location = element.offset;
    }
    device->scan_size = *stride;
    return true;
}

void IioBackend::WriteSamplingFrequency(IioDeviceState* device,
                                        const std::vector<IioSensorData*>& active) {
    if (active.empty()) return;
    int64_t period = active.front()->sampling_period_ns.load();
    for (const auto* sensor : active) period = std::min(period, sensor->sampling_period_ns.load());
    if (period <= 0) period = kDefaultPeriodNs;
    int64_t physical_period = period;
    if (device->device_name.find("qcom-smgr") != std::string::npos) {
        double maximum_frequency = 0.0;
        for (const auto* sensor : active) {
            std::string available =
                    ReadString(device->sysfs_path + "/in_" + TypePrefix(sensor->type) +
                               "_sampling_frequency_available");
            for (double frequency : ParseFrequencies(available))
                maximum_frequency = std::max(maximum_frequency, frequency);
        }
        if (maximum_frequency > 0.0)
            physical_period = static_cast<int64_t>(kNsPerSecond / maximum_frequency);
    }
    device->physical_period_ns = std::max<int64_t>(physical_period, 1);
    std::ostringstream value;
    value << std::setprecision(12) << static_cast<double>(kNsPerSecond) / period;
    const std::string common = device->sysfs_path + "/sampling_frequency";
    if (Exists(common)) WriteString(common, value.str());
    for (const auto* sensor : active) {
        const std::string typed =
                device->sysfs_path + "/in_" + TypePrefix(sensor->type) + "_sampling_frequency";
        if (Exists(typed)) WriteString(typed, value.str());
    }
    if (device->hrtimer_trigger) {
        std::error_code error;
        std::filesystem::directory_iterator entries(kIioPath, error);
        for (const auto& entry : entries) {
            if (ReadString(entry.path().string() + "/name") == device->trigger_name) {
                WriteString(entry.path().string() + "/sampling_frequency", value.str());
                break;
            }
        }
    }
}

bool IioBackend::TryEnableBuffer(IioDeviceState* device) {
    if (!WriteString(device->sysfs_path + "/buffer/enable", "1")) return false;
    const std::string dev = "/dev/iio:device" + std::to_string(device->dev_num);
    device->buffer_fd.reset(open(dev.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!device->buffer_fd.ok() || pipe2(device->signal_pipe_fd, O_CLOEXEC | O_NONBLOCK) != 0) {
        DisableBuffer(device);
        device->buffer_fd.reset();
        return false;
    }
    return true;
}

bool IioBackend::UseExistingOrDeviceTrigger(IioDeviceState* device) {
    const std::string current_path = device->sysfs_path + "/trigger/current_trigger";
    device->trigger_name = ReadString(current_path);
    if (!device->trigger_name.empty()) return true;
    std::error_code error;
    std::filesystem::directory_iterator entries(kIioPath, error);
    if (error) return false;
    const std::string dev_pattern = "dev" + std::to_string(device->dev_num);
    for (const auto& entry : entries) {
        if (!entry.path().filename().string().starts_with("trigger")) continue;
        const std::string trigger = ReadString(entry.path().string() + "/name");
        if (trigger.find(device->device_name) == std::string::npos &&
            trigger.find(dev_pattern) == std::string::npos)
            continue;
        if (WriteString(current_path, trigger)) {
            device->trigger_name = trigger;
            device->trigger_attached_by_hal = true;
            return true;
        }
    }
    return false;
}

bool IioBackend::CreateHrtimerTrigger(IioDeviceState* device) {
    if (!Exists(kHrtimerPath)) return false;
    const std::string name = "mainline_sensors_" + std::to_string(device->dev_num);
    const std::string path = std::string(kHrtimerPath) + "/" + name;
    std::error_code error;
    const bool created = std::filesystem::create_directory(path, error);
    if (!created && error) return false;
    if (!WriteString(device->sysfs_path + "/trigger/current_trigger", name)) {
        if (created) std::filesystem::remove(path, error);
        return false;
    }
    device->trigger_name = name;
    device->trigger_attached_by_hal = true;
    device->hrtimer_trigger = true;
    device->hrtimer_owned = created;
    return true;
}

bool IioBackend::ConfigureBuffer(IioDeviceState* device,
                                 const std::vector<IioSensorData*>& active) {
    DisableBuffer(device);
    if (!ConfigureScanMask(device, active)) return false;
    WriteString(device->sysfs_path + "/buffer/length", std::to_string(kBufferLength));
    if (Exists(device->sysfs_path + "/current_timestamp_clock"))
        WriteString(device->sysfs_path + "/current_timestamp_clock", "boottime");
    WriteSamplingFrequency(device, active);

    if (UseExistingOrDeviceTrigger(device)) {
        if (TryEnableBuffer(device)) return true;
        if (device->trigger_attached_by_hal) {
            ReleaseOwnedTrigger(device);
        } else {
            // Never replace or detach a trigger assigned by the kernel or another user.
            return false;
        }
    }
    // Push-buffer drivers such as qcom-smgr do not need a trigger.
    if (TryEnableBuffer(device)) return true;
    if (CreateHrtimerTrigger(device)) {
        WriteSamplingFrequency(device, active);
        if (TryEnableBuffer(device)) return true;
        ReleaseOwnedTrigger(device);
    }
    return false;
}

void IioBackend::StartReader(const std::shared_ptr<IioDeviceState>& device) {
    device->stop_reader = false;
    device->reader_running = true;
    device->reader_thread = std::thread(&IioBackend::ReaderThread, this, device);
}

bool IioBackend::ReconfigureDevice(const std::shared_ptr<IioDeviceState>& device) {
    std::lock_guard config_lock(device->config_mutex);
    StopReader(device.get());
    DisableBuffer(device.get());
    std::vector<IioSensorData*> active;
    {
        std::lock_guard lock(device->mutex);
        for (auto* sensor : device->sensors) {
            if (sensor->enabled && !sensor->direct_poll) active.push_back(sensor);
        }
    }
    if (active.empty()) {
        ReleaseOwnedTrigger(device.get());
        return true;
    }
    if (ConfigureBuffer(device.get(), active)) {
        StartReader(device);
        return true;
    }
    LOG(WARNING) << "Buffered I/O unavailable for " << device->device_name;
    return false;
}

std::optional<Event> IioBackend::BuildEvent(IioSensorData* sensor, std::vector<float> values,
                                            int64_t timestamp) {
    if (values.empty()) return std::nullopt;
    std::lock_guard lock(sensor->event_mutex);
    if (IsOnChange(sensor->type) && sensor->last_value == values) return std::nullopt;
    const int64_t period = sensor->sampling_period_ns.load();
    if (!IsOnChange(sensor->type) && sensor->last_delivered_timestamp_ns != 0 &&
        timestamp - sensor->last_delivered_timestamp_ns < period)
        return std::nullopt;
    sensor->last_value = values;
    sensor->last_delivered_timestamp_ns = timestamp;
    Event event;
    event.sensorHandle = sensor->handle;
    event.sensorType = sensor->type;
    event.timestamp = timestamp;
    if (IsVector(sensor->type)) {
        EventPayload::Vec3 vector = {.x = values[0],
                                     .y = values[1],
                                     .z = values[2],
                                     .status = SensorStatus::ACCURACY_HIGH};
        event.payload.set<EventPayload::Tag::vec3>(vector);
    } else {
        event.payload.set<EventPayload::Tag::scalar>(values[0]);
    }
    return event;
}

std::vector<Event> IioBackend::ParseScans(IioSensorData* sensor, const uint8_t* data, size_t scans,
                                          const std::vector<int64_t>& timestamps) {
    std::vector<Event> events;
    const auto& effective = sensor->device->effective_channels;
    for (size_t scan = 0; scan < scans; ++scan) {
        std::vector<float> values;
        for (const auto& wanted : sensor->channels) {
            const auto found =
                    std::find_if(effective.begin(), effective.end(),
                                 [&](const auto& item) { return item.name == wanted.name; });
            if (found == effective.end()) break;
            const auto decoded = iio::DecodeScanValue(
                    data + scan * sensor->device->scan_size + found->location,
                    sensor->device->scan_size - found->location, found->scan_type);
            if (!decoded) break;
            double value;
            if (sensor->type == SensorType::PROXIMITY && sensor->proximity_near_level >= 0) {
                value = decoded->AsDouble() >= sensor->proximity_near_level
                                ? 0.0
                                : sensor->sensor_info.maxRange;
            } else {
                value = (decoded->AsDouble() + found->offset) * found->scale;
                value = iio::ConvertUnit(value, UnitForType(sensor->type));
            }
            values.push_back(static_cast<float>(value));
        }
        if (values.size() != sensor->channels.size()) continue;
        if (IsVector(sensor->type)) {
            std::vector<float> mounted(3);
            for (size_t row = 0; row < 3; ++row) {
                mounted[row] = sensor->mount_matrix[row * 3] * values[0] +
                               sensor->mount_matrix[row * 3 + 1] * values[1] +
                               sensor->mount_matrix[row * 3 + 2] * values[2];
            }
            values = std::move(mounted);
        }
        if (auto event = BuildEvent(sensor, std::move(values), timestamps[scan]))
            events.push_back(*event);
    }
    return events;
}

void IioBackend::PostEvents(const std::vector<Event>& events, bool wakeup) {
    if (events.empty() || operation_mode_ == OperationMode::DATA_INJECTION) return;
    PostEventsCallback callback;
    {
        std::lock_guard lock(callback_mutex_);
        callback = callback_;
    }
    if (callback) callback(events, wakeup);
}

void IioBackend::PostFlushes(const std::vector<int32_t>& handles) {
    std::vector<Event> events;
    for (int32_t handle : handles) {
        Event event;
        event.sensorHandle = handle;
        event.sensorType = SensorType::META_DATA;
        EventPayload::MetaData meta = {
                .what = EventPayload::MetaData::MetaDataEventType::META_DATA_FLUSH_COMPLETE};
        event.payload.set<EventPayload::Tag::meta>(meta);
        events.push_back(event);
    }
    PostEvents(events, false);
}

void IioBackend::ReaderThread(std::shared_ptr<IioDeviceState> device) {
    std::vector<uint8_t> input(device->scan_size * kBufferLength);
    pollfd fds[2] = {{device->buffer_fd.get(), POLLIN, 0}, {device->signal_pipe_fd[0], POLLIN, 0}};
    while (!device->stop_reader) {
        const int ready = poll(fds, 2, 1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (fds[0].revents & POLLIN) {
            const ssize_t count = read(device->buffer_fd.get(), input.data(), input.size());
            if (count > 0) {
                device->partial_data.insert(device->partial_data.end(), input.begin(),
                                            input.begin() + count);
                const size_t scans = device->partial_data.size() / device->scan_size;
                if (scans > 0) {
                    const int64_t received = BoottimeNs();
                    int64_t sample_period = device->physical_period_ns.load();
                    std::vector<IioSensorData*> active;
                    {
                        std::lock_guard lock(device->mutex);
                        for (auto* sensor : device->sensors)
                            if (sensor->enabled && !sensor->direct_poll) active.push_back(sensor);
                    }
                    if (device->last_receive_timestamp_ns != 0 &&
                        received > device->last_receive_timestamp_ns) {
                        const int64_t observed = (received - device->last_receive_timestamp_ns) /
                                                 static_cast<int64_t>(scans);
                        if (observed > 0 && observed < sample_period * 4) sample_period = observed;
                    }
                    device->last_receive_timestamp_ns = received;
                    const auto timestamp_channel = std::find_if(
                            device->effective_channels.begin(), device->effective_channels.end(),
                            [](const auto& channel) {
                                return channel.name.find("timestamp") != std::string::npos;
                            });
                    std::vector<int64_t> timestamps(scans);
                    for (size_t scan = 0; scan < scans; ++scan) {
                        int64_t timestamp =
                                received - static_cast<int64_t>(scans - scan - 1) * sample_period;
                        if (timestamp_channel != device->effective_channels.end() &&
                            timestamp_channel->scan_type.real_bits == 64) {
                            const auto decoded = iio::DecodeScanValue(
                                    device->partial_data.data() + scan * device->scan_size +
                                            timestamp_channel->location,
                                    device->scan_size - timestamp_channel->location,
                                    timestamp_channel->scan_type);
                            if (decoded &&
                                decoded->bits <= static_cast<uint64_t>(received + kNsPerSecond) &&
                                decoded->bits + 86400ULL * kNsPerSecond >=
                                        static_cast<uint64_t>(received)) {
                                timestamp = static_cast<int64_t>(decoded->bits);
                            }
                        }
                        timestamp = std::max(timestamp, device->last_timestamp_ns + 1);
                        timestamps[scan] = timestamp;
                        device->last_timestamp_ns = timestamp;
                    }
                    for (auto* sensor : active) {
                        const auto events =
                                ParseScans(sensor, device->partial_data.data(), scans, timestamps);
                        const bool wakeup =
                                sensor->sensor_info.flags &
                                static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP);
                        PostEvents(events, wakeup);
                    }
                    device->partial_data.erase(
                            device->partial_data.begin(),
                            device->partial_data.begin() + scans * device->scan_size);
                }
            }
        }
        std::vector<int32_t> flushes;
        if (fds[1].revents & POLLIN) {
            uint8_t drain[32];
            while (read(device->signal_pipe_fd[0], drain, sizeof(drain)) > 0) {
            }
            if (!device->stop_reader) {
                std::lock_guard lock(device->mutex);
                flushes.swap(device->pending_flushes);
            }
        }
        PostFlushes(flushes);
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
    }
    device->reader_running = false;
}

std::optional<Event> IioBackend::ReadDirectEvent(IioSensorData* sensor) {
    std::vector<float> values;
    for (const auto& source : sensor->direct_sources) {
        const double read = ReadDouble(source.path, std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(read)) return std::nullopt;
        double value;
        if (sensor->type == SensorType::PROXIMITY && sensor->proximity_near_level >= 0 &&
            !source.is_input) {
            value = read >= sensor->proximity_near_level ? 0.0 : sensor->sensor_info.maxRange;
        } else {
            value = source.is_input ? read : (read + source.offset) * source.scale;
            value = iio::ConvertUnit(value, UnitForType(sensor->type));
        }
        values.push_back(value);
    }
    if (IsVector(sensor->type)) {
        std::vector<float> mounted(3);
        for (size_t row = 0; row < 3; ++row)
            mounted[row] = sensor->mount_matrix[row * 3] * values[0] +
                           sensor->mount_matrix[row * 3 + 1] * values[1] +
                           sensor->mount_matrix[row * 3 + 2] * values[2];
        values = std::move(mounted);
    }
    return BuildEvent(sensor, std::move(values), BoottimeNs());
}

void IioBackend::PollThread(IioSensorData* sensor) {
    while (!sensor->stop_thread) {
        if (sensor->enabled && operation_mode_ == OperationMode::NORMAL) {
            if (auto event = ReadDirectEvent(sensor)) {
                const bool wakeup = sensor->sensor_info.flags &
                                    static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP);
                PostEvents({*event}, wakeup);
            }
        }
        std::unique_lock lock(sensor->poll_mutex);
        const int64_t period = sensor->sampling_period_ns.load();
        sensor->poll_cv.wait_for(
                lock, std::chrono::nanoseconds(std::max<int64_t>(period, 1000000LL)), [&] {
                    return sensor->stop_thread.load() || !sensor->enabled.load() ||
                           sensor->sampling_period_ns.load() != period;
                });
    }
}

int32_t IioBackend::Activate(int32_t handle, bool enabled) {
    std::lock_guard backend_lock(mutex_);
    std::shared_ptr<IioSensorData> sensor;
    const auto found = sensors_.find(handle);
    if (found == sensors_.end()) return -EINVAL;
    sensor = found->second;
    bool direct;
    {
        std::lock_guard lifecycle_lock(sensor->lifecycle_mutex);
        if (sensor->enabled.exchange(enabled) == enabled) return 0;
        {
            std::lock_guard event_lock(sensor->event_mutex);
            sensor->last_value.reset();
            sensor->last_delivered_timestamp_ns = 0;
        }
        direct = sensor->direct_poll;
        if (!enabled && direct) {
            sensor->stop_thread = true;
            sensor->poll_cv.notify_all();
            if (sensor->poll_thread.joinable()) sensor->poll_thread.join();
        }
        if (enabled && direct) {
            if (sensor->direct_sources.empty()) {
                sensor->enabled = false;
                return -ENODEV;
            }
            sensor->stop_thread = false;
            if (!sensor->poll_thread.joinable())
                sensor->poll_thread = std::thread(&IioBackend::PollThread, this, sensor);
        }
    }
    if (sensor->device && !direct) {
        if (!ReconfigureDevice(sensor->device) && enabled) {
            sensor->enabled = false;
            // Restore the configuration used by sensors that were active before this request.
            ReconfigureDevice(sensor->device);
            if (sensor->direct_sources.empty()) return -ENODEV;

            std::lock_guard lifecycle_lock(sensor->lifecycle_mutex);
            sensor->direct_poll = true;
            sensor->enabled = true;
            sensor->stop_thread = false;
            if (!sensor->poll_thread.joinable())
                sensor->poll_thread = std::thread(&IioBackend::PollThread, this, sensor.get());
        }
    }
    return 0;
}

int32_t IioBackend::Batch(int32_t handle, int64_t period_ns, int64_t latency_ns) {
    if (period_ns < 0 || latency_ns < 0) return -EINVAL;
    std::lock_guard backend_lock(mutex_);
    std::shared_ptr<IioSensorData> sensor;
    const auto found = sensors_.find(handle);
    if (found == sensors_.end()) return -EINVAL;
    sensor = found->second;
    int64_t minimum = static_cast<int64_t>(sensor->sensor_info.minDelayUs) * 1000;
    int64_t maximum = static_cast<int64_t>(sensor->sensor_info.maxDelayUs) * 1000;
    if (minimum > 0) period_ns = std::max(period_ns, minimum);
    if (maximum > 0) period_ns = std::min(period_ns, maximum);
    if (period_ns == 0) period_ns = kDefaultPeriodNs;
    sensor->sampling_period_ns = period_ns;
    sensor->poll_cv.notify_all();
    if (sensor->enabled && sensor->device && !sensor->direct_poll) {
        std::lock_guard config_lock(sensor->device->config_mutex);
        std::vector<IioSensorData*> active;
        {
            std::lock_guard device_lock(sensor->device->mutex);
            for (auto* candidate : sensor->device->sensors)
                if (candidate->enabled && !candidate->direct_poll) active.push_back(candidate);
        }
        WriteSamplingFrequency(sensor->device.get(), active);
    }
    return 0;
}

int32_t IioBackend::Flush(int32_t handle) {
    std::lock_guard backend_lock(mutex_);
    std::shared_ptr<IioSensorData> sensor;
    const auto found = sensors_.find(handle);
    if (found == sensors_.end() || !found->second->enabled) return -EINVAL;
    sensor = found->second;
    bool queued = false;
    if (sensor->device && !sensor->direct_poll) {
        std::lock_guard config_lock(sensor->device->config_mutex);
        if (sensor->device->reader_running) {
            {
                std::lock_guard lock(sensor->device->mutex);
                sensor->device->pending_flushes.push_back(handle);
            }
            const uint8_t signal = 2;
            (void)write(sensor->device->signal_pipe_fd[1], &signal, sizeof(signal));
            queued = true;
        }
    }
    if (!queued) PostFlushes({handle});
    return 0;
}

int32_t IioBackend::SetOperationMode(OperationMode mode) {
    std::lock_guard lock(mutex_);
    operation_mode_ = mode;
    for (auto& [handle, sensor] : sensors_) sensor->poll_cv.notify_all();
    return 0;
}

}  // namespace aidl::android::hardware::sensors::mainline
