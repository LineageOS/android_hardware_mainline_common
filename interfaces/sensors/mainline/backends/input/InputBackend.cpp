/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsInput"

#include "InputBackend.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/Sysfs.h>

#include <algorithm>
#include <cerrno>

namespace aidl::android::hardware::sensors::mainline {

namespace {
constexpr const char* kInputDevDir = "/dev/input";
}  // namespace

DEFINE_SENSOR_BACKEND(InputBackend, 0)

InputBackend::InputBackend() = default;

InputBackend::~InputBackend() {
    Deinitialize();
}

std::string InputBackend::GetName() const {
    return "input";
}

int32_t InputBackend::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lock(mutex_);

    hwdb_ = SensorHwdb::Load();

    std::vector<std::string> entries = sysfs::ListDirectory(kInputDevDir);
    // Sort numerically so that handles are stable for a given set of devices.
    std::sort(entries.begin(), entries.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    for (const auto& entry : entries) {
        if (!::android::base::StartsWith(entry, "event")) {
            continue;
        }
        std::unique_ptr<InputDevice> device = InputDevice::Probe(entry, &next_handle_, hwdb_.get());
        if (!device) {
            continue;
        }
        device->SetCallback(callback);
        for (const auto& info : device->GetSensorInfos()) {
            LOG(INFO) << "Input sensor: " << SensorInfoToString(info) << " (" << entry << ")";
        }
        devices_.push_back(std::move(device));
    }

    size_t count = 0;
    for (const auto& device : devices_) {
        count += device->GetSensorInfos().size();
    }
    LOG(INFO) << "Input backend initialized with " << count << " sensor(s) on " << devices_.size()
              << " device(s)";
    return 0;
}

void InputBackend::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& device : devices_) {
        device->Shutdown();
        device->SetCallback(nullptr);
    }
    LOG(INFO) << "Input backend deinitialized";
}

std::vector<SensorInfo> InputBackend::GetSensorsList() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> list;
    for (const auto& device : devices_) {
        for (const auto& info : device->GetSensorInfos()) {
            list.push_back(info);
        }
    }
    return list;
}

InputDevice* InputBackend::FindDevice(int32_t handle) {
    for (auto& device : devices_) {
        if (device->HasSensor(handle)) {
            return device.get();
        }
    }
    return nullptr;
}

int32_t InputBackend::Activate(int32_t sensor_handle, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    InputDevice* device = FindDevice(sensor_handle);
    if (device == nullptr) {
        return -EINVAL;
    }
    int32_t ret = device->Activate(sensor_handle, enabled);
    LOG(INFO) << "Input sensor " << sensor_handle << (enabled ? " activated" : " deactivated")
              << " (" << device->GetPath() << ") -> " << ret;
    return ret;
}

int32_t InputBackend::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                            int64_t /* max_report_latency_ns */) {
    std::lock_guard<std::mutex> lock(mutex_);
    InputDevice* device = FindDevice(sensor_handle);
    if (device == nullptr) {
        return -EINVAL;
    }
    LOG(DEBUG) << "Input sensor " << sensor_handle << " period " << sampling_period_ns / 1000
               << " us";
    return device->Batch(sensor_handle, sampling_period_ns);
}

int32_t InputBackend::Flush(int32_t sensor_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (FindDevice(sensor_handle) == nullptr) {
        return -EINVAL;
    }
    return kFlushHandledByFrontend;
}

int32_t InputBackend::SetOperationMode(OperationMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& device : devices_) {
        device->SetPaused(mode != OperationMode::NORMAL);
    }
    LOG(INFO) << "Input backend operation mode " << toString(mode);
    return 0;
}

}  // namespace aidl::android::hardware::sensors::mainline
