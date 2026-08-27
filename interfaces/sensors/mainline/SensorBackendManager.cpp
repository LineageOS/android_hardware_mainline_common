/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsBackendManager"

#include "SensorBackendManager.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <dlfcn.h>

#include <cerrno>
#include <set>

namespace aidl::android::hardware::sensors::mainline {

static constexpr const char* kBackendProperty = "vendor.sensors.backends";

static constexpr const char* kKnownBackends[] = {
        "libsensors_iio.so",
        "libsensors_input.so",
        "libsensors_mock.so",
};

static constexpr const char* kLibrarySearchPaths[] = {
        "",
#ifdef __LP64__
        "/apex/com.android.hardware.sensors.mainline/lib64/",
        "/odm/lib64/hw/",
        "/vendor/lib64/hw/",
#else
        "/apex/com.android.hardware.sensors.mainline/lib/",
        "/odm/lib/hw/",
        "/vendor/lib/hw/",
#endif
};

SensorBackendManager::SensorBackendManager() = default;

SensorBackendManager::~SensorBackendManager() {
    Deinitialize();
    for (auto& entry : backends_) {
        if (entry.dl_handle != nullptr) {
            dlclose(entry.dl_handle);
            entry.dl_handle = nullptr;
        }
    }
}

std::vector<std::string> SensorBackendManager::GetBackendList() {
    std::vector<std::string> backends;
    std::set<std::string> seen;

    std::string override_list = ::android::base::GetProperty(kBackendProperty, "");
    if (!override_list.empty()) {
        LOG(INFO) << "Backend list override: " << override_list;
        std::string token;
        for (size_t i = 0; i <= override_list.size(); i++) {
            if (i == override_list.size() || override_list[i] == ' ' || override_list[i] == ',') {
                if (!token.empty() && seen.find(token) == seen.end()) {
                    backends.push_back(token);
                    seen.insert(token);
                }
                token.clear();
            } else {
                token += override_list[i];
            }
        }
    }

    for (const auto& name : kKnownBackends) {
        if (seen.find(name) == seen.end()) {
            backends.push_back(name);
            seen.insert(name);
        }
    }

    return backends;
}

void SensorBackendManager::LoadBackend(const std::string& library_name) {
    void* handle = nullptr;

    for (const auto& path : kLibrarySearchPaths) {
        std::string full_path = std::string(path) + library_name;
        handle = dlopen(full_path.c_str(), RTLD_NOW);
        if (handle != nullptr) {
            LOG(INFO) << "Loaded backend library: " << full_path;
            break;
        }
        LOG(DEBUG) << "Failed to load " << full_path << ": " << dlerror();
    }

    if (handle == nullptr) {
        LOG(WARNING) << "Could not load backend library: " << library_name;
        return;
    }

    dlerror();
    auto create_func = reinterpret_cast<CreateBackendFunc>(dlsym(handle, kCreateBackendSymbol));
    if (create_func == nullptr) {
        LOG(WARNING) << "Backend library " << library_name << " missing symbol "
                     << kCreateBackendSymbol << ": " << dlerror();
        dlclose(handle);
        return;
    }

    ISensorBackend* backend = create_func();
    if (backend == nullptr) {
        LOG(WARNING) << "Backend library " << library_name << " factory returned nullptr";
        dlclose(handle);
        return;
    }

    BackendEntry entry;
    entry.name = backend->GetName();
    entry.dl_handle = handle;
    entry.backend.reset(backend);

    LOG(INFO) << "Backend '" << entry.name << "' created from " << library_name;
    backends_.push_back(std::move(entry));
}

void SensorBackendManager::LoadBackends() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto backend_list = GetBackendList();
    for (const auto& name : backend_list) {
        LoadBackend(name);
    }

    LOG(INFO) << "Loaded " << backends_.size() << " backend(s)";
}

void SensorBackendManager::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    post_events_callback_ = callback;

    for (size_t i = 0; i < backends_.size(); i++) {
        auto& entry = backends_[i];

        auto wrapped_callback = [this, i](const std::vector<Event>& events, bool wakeup) {
            std::vector<Event> remapped_events = events;
            std::lock_guard<std::mutex> cb_lock(mutex_);
            if (i < backends_.size()) {
                for (auto& ev : remapped_events) {
                    auto it = backends_[i].local_to_global_handles.find(ev.sensorHandle);
                    if (it != backends_[i].local_to_global_handles.end()) {
                        ev.sensorHandle = it->second;
                    }
                }
            }
            if (post_events_callback_) {
                post_events_callback_(remapped_events, wakeup);
            }
        };

        int32_t result = entry.backend->Initialize(wrapped_callback);
        if (result != 0) {
            LOG(WARNING) << "Backend '" << entry.name << "' failed to initialize: " << result;
            continue;
        }

        auto sensors = entry.backend->GetSensorsList();
        for (auto& sensor_info : sensors) {
            int32_t local_handle = sensor_info.sensorHandle;
            int32_t global_handle = next_handle_++;

            entry.local_to_global_handles[local_handle] = global_handle;
            entry.global_to_local_handles[global_handle] = local_handle;
            global_handle_to_backend_[global_handle] = i;

            sensor_info.sensorHandle = global_handle;

            LOG(INFO) << "Sensor [backend='" << entry.name << "'] handle=" << global_handle
                      << " type=" << static_cast<int32_t>(sensor_info.type)
                      << " name='" << sensor_info.name << "' vendor='" << sensor_info.vendor
                      << "'";
        }
    }
}

void SensorBackendManager::Deinitialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : backends_) {
        entry.backend->Deinitialize();
    }
    post_events_callback_ = nullptr;
}

std::vector<SensorInfo> SensorBackendManager::GetSensorsList() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> all_sensors;

    for (size_t i = 0; i < backends_.size(); i++) {
        auto sensors = backends_[i].backend->GetSensorsList();
        for (auto& sensor_info : sensors) {
            auto it = backends_[i].local_to_global_handles.find(sensor_info.sensorHandle);
            if (it != backends_[i].local_to_global_handles.end()) {
                sensor_info.sensorHandle = it->second;
                all_sensors.push_back(sensor_info);
            }
        }
    }

    return all_sensors;
}

int32_t SensorBackendManager::GetBackendIndex(int32_t global_handle) {
    auto it = global_handle_to_backend_.find(global_handle);
    if (it == global_handle_to_backend_.end()) {
        return -1;
    }
    return static_cast<int32_t>(it->second);
}

int32_t SensorBackendManager::Activate(int32_t sensor_handle, bool enabled) {
    ISensorBackend* backend = nullptr;
    int32_t local_handle = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t idx = GetBackendIndex(sensor_handle);
        if (idx < 0 || static_cast<size_t>(idx) >= backends_.size()) {
            return -EINVAL;
        }
        auto& entry = backends_[idx];
        auto it = entry.global_to_local_handles.find(sensor_handle);
        if (it == entry.global_to_local_handles.end()) {
            return -EINVAL;
        }
        backend = entry.backend.get();
        local_handle = it->second;
    }
    return backend->Activate(local_handle, enabled);
}

int32_t SensorBackendManager::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                                    int64_t max_report_latency_ns) {
    ISensorBackend* backend = nullptr;
    int32_t local_handle = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t idx = GetBackendIndex(sensor_handle);
        if (idx < 0 || static_cast<size_t>(idx) >= backends_.size()) {
            return -EINVAL;
        }
        auto& entry = backends_[idx];
        auto it = entry.global_to_local_handles.find(sensor_handle);
        if (it == entry.global_to_local_handles.end()) {
            return -EINVAL;
        }
        backend = entry.backend.get();
        local_handle = it->second;
    }
    return backend->Batch(local_handle, sampling_period_ns, max_report_latency_ns);
}

int32_t SensorBackendManager::Flush(int32_t sensor_handle) {
    ISensorBackend* backend = nullptr;
    int32_t local_handle = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t idx = GetBackendIndex(sensor_handle);
        if (idx < 0 || static_cast<size_t>(idx) >= backends_.size()) {
            return -EINVAL;
        }
        auto& entry = backends_[idx];
        auto it = entry.global_to_local_handles.find(sensor_handle);
        if (it == entry.global_to_local_handles.end()) {
            return -EINVAL;
        }
        backend = entry.backend.get();
        local_handle = it->second;
    }
    return backend->Flush(local_handle);
}

int32_t SensorBackendManager::SetOperationMode(OperationMode mode) {
    std::vector<ISensorBackend*> backends;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : backends_) {
            backends.push_back(entry.backend.get());
        }
    }
    int32_t last_result = 0;
    for (auto* backend : backends) {
        int32_t result = backend->SetOperationMode(mode);
        if (result != 0) {
            LOG(WARNING) << "Backend SetOperationMode failed: " << result;
            last_result = result;
        }
    }
    return last_result;
}

}  // namespace aidl::android::hardware::sensors::mainline
