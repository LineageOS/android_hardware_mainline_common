/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsIio"

#include "IioBackend.h"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/Settings.h>
#include <libsensors_common/Sysfs.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <thread>

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr const char* kIioDevicesDir = "/sys/bus/iio/devices";
constexpr const char* kDevicePrefix = "iio:device";
constexpr int kDiscoveryPollMs = 200;

std::vector<std::pair<int, std::string>> ListIioDevices() {
    std::vector<std::pair<int, std::string>> devices;
    for (const auto& entry : sysfs::ListDirectory(kIioDevicesDir)) {
        if (!::android::base::StartsWith(entry, kDevicePrefix)) {
            continue;
        }
        int dev_num = -1;
        if (!::android::base::ParseInt(entry.substr(strlen(kDevicePrefix)), &dev_num)) {
            continue;
        }
        devices.emplace_back(dev_num, std::string(kIioDevicesDir) + "/" + entry);
    }
    // Deterministic handle assignment.
    std::sort(devices.begin(), devices.end());
    return devices;
}

}  // namespace

DEFINE_SENSOR_BACKEND(IioBackend, 0)

IioBackend::IioBackend() = default;

IioBackend::~IioBackend() {
    Deinitialize();
}

std::string IioBackend::GetName() const {
    return "iio";
}

void IioBackend::WaitForDevices() {
    // Some sensors show up late (e.g. Qualcomm SMGR sensors appear once the
    // DSP firmware is up). Optionally wait for the first IIO device.
    const int64_t wait_ms = Settings::Get().GetInt("iio.discovery_wait_ms", 0);
    if (wait_ms <= 0) {
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (ListIioDevices().empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kDiscoveryPollMs));
    }
    const int64_t settle_ms = Settings::Get().GetInt("iio.discovery_settle_ms", 0);
    if (settle_ms > 0 && !ListIioDevices().empty()) {
        // Give sibling devices of the same subsystem time to appear.
        std::this_thread::sleep_for(std::chrono::milliseconds(settle_ms));
    }
}

void IioBackend::DiscoverDevices() {
    for (const auto& [dev_num, path] : ListIioDevices()) {
        std::unique_ptr<IioDevice> device =
                IioDevice::Discover(dev_num, path, hwdb_.get(), &next_handle_);
        if (!device) {
            continue;
        }
        for (IioSensor* sensor : device->GetSensors()) {
            handle_to_device_[sensor->GetHandle()] = device.get();
        }
        devices_.push_back(std::move(device));
    }
}

void IioBackend::MakeSensorNamesUnique() {
    // Android requires unique names among sensors of the same type.
    std::map<std::pair<SensorType, std::string>, int> seen;
    for (auto& device : devices_) {
        for (IioSensor* sensor : device->GetSensors()) {
            SensorInfo* info = sensor->MutableInfo();
            seen[{info->type, info->name}]++;
        }
    }
    for (auto& device : devices_) {
        for (IioSensor* sensor : device->GetSensors()) {
            SensorInfo* info = sensor->MutableInfo();
            if (seen[{info->type, info->name}] > 1) {
                std::string unique = info->name + " (iio:device" +
                                     std::to_string(device->GetInfo().dev_num) + ")";
                LOG(INFO) << "Renaming duplicate sensor '" << info->name << "' to '" << unique
                          << "'";
                info->name = unique;
            }
        }
    }
}

int32_t IioBackend::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    IioTrigger::CleanupStaleTriggers();
    hwdb_ = SensorHwdb::Load();

    WaitForDevices();
    DiscoverDevices();
    MakeSensorNamesUnique();

    size_t count = 0;
    for (auto& device : devices_) {
        device->SetCallback(callback);
        for (IioSensor* sensor : device->GetSensors()) {
            LOG(INFO) << "IIO sensor: " << sensor->Describe();
            count++;
        }
    }
    LOG(INFO) << "IIO backend initialized with " << count << " sensor(s) on " << devices_.size()
              << " device(s)";
    return 0;
}

void IioBackend::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& device : devices_) {
        device->Shutdown();
        device->SetCallback(nullptr);
    }
    LOG(INFO) << "IIO backend deinitialized";
}

std::vector<SensorInfo> IioBackend::GetSensorsList() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> list;
    for (auto& device : devices_) {
        for (IioSensor* sensor : device->GetSensors()) {
            list.push_back(sensor->GetInfo());
        }
    }
    return list;
}

IioDevice* IioBackend::FindDevice(int32_t handle) {
    auto it = handle_to_device_.find(handle);
    return it == handle_to_device_.end() ? nullptr : it->second;
}

int32_t IioBackend::Activate(int32_t sensor_handle, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    IioDevice* device = FindDevice(sensor_handle);
    if (device == nullptr) {
        return -EINVAL;
    }
    return device->Activate(sensor_handle, enabled);
}

int32_t IioBackend::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                          int64_t /* max_report_latency_ns */) {
    std::lock_guard<std::mutex> lock(mutex_);
    IioDevice* device = FindDevice(sensor_handle);
    if (device == nullptr) {
        return -EINVAL;
    }
    return device->SetPeriod(sensor_handle, sampling_period_ns);
}

int32_t IioBackend::Flush(int32_t sensor_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (FindDevice(sensor_handle) == nullptr) {
        return -EINVAL;
    }
    // Samples are delivered as soon as they are read from the kernel; there
    // is no HAL side FIFO to flush.
    return kFlushHandledByFrontend;
}

int32_t IioBackend::SetOperationMode(OperationMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& device : devices_) {
        device->SetPaused(mode != OperationMode::NORMAL);
    }
    LOG(INFO) << "IIO backend operation mode " << toString(mode);
    return 0;
}

}  // namespace aidl::android::hardware::sensors::mainline
