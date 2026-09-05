/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsBackendManager"

#include "SensorBackendManager.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <dlfcn.h>

#include <algorithm>
#include <cerrno>
#include <set>
#include <tuple>

namespace aidl::android::hardware::sensors::mainline {

static constexpr const char* kBackendProperty = "vendor.sensors.backends";

static const std::vector<std::string> kKnownBackends = {
        "libsensors_iio.so",
        "libsensors_input.so",
        "libsensors_mock.so",
};

static std::string ExpandBackendName(const std::string& name) {
    return "libsensors_" + name + ".so";
}

static constexpr const char* kLibrarySearchPaths[] = {
        "",
#ifdef __LP64__
        "/apex/com.android.hardware.sensors/lib64/",
        "/odm/lib64/hw/",
        "/vendor/lib64/hw/",
#else
        "/apex/com.android.hardware.sensors/lib/",
        "/odm/lib/hw/",
        "/vendor/lib/hw/",
#endif
};

SensorBackendManager::SensorBackendManager() = default;

SensorBackendManager::~SensorBackendManager() {
    Deinitialize();
    for (auto& entry : backends_) {
        entry.backend.reset();
        if (entry.dl_handle != nullptr) {
            dlclose(entry.dl_handle);
            entry.dl_handle = nullptr;
        }
    }
}

void SensorBackendManager::RegisterCompositeSensor(std::unique_ptr<ICompositeSensor> sensor) {
    composite_sensors_.push_back(std::move(sensor));
}

std::vector<std::string> SensorBackendManager::GetBackendList() {
    std::vector<std::string> backends;

    std::string override_list =
            ::android::base::GetProperty(kBackendProperty, LOAD_CUSTOM_BACKENDS);
    if (!override_list.empty()) {
        LOG(INFO) << "Backend list override: " << override_list;
        backends = ::android::base::Split(override_list, ",");
        for (auto& backend : backends) {
            backend = ExpandBackendName(backend);
        }
        return backends;
    }

    return kKnownBackends;
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

std::vector<Event> SensorBackendManager::ProcessCompositeSensors(const std::vector<Event>& events) {
    std::vector<Event> all_composite_events;

    for (const auto& event : events) {
        for (auto& composite : composite_sensors_) {
            if (!composite->IsActive()) {
                continue;
            }

            auto input_types = composite->GetInputSensorTypes();
            bool interested = false;
            for (const auto& type : input_types) {
                if (type == event.sensorType) {
                    interested = true;
                    break;
                }
            }

            if (interested) {
                auto output = composite->ProcessEvent(event);
                all_composite_events.insert(all_composite_events.end(), output.begin(),
                                            output.end());
            }
        }
    }

    return all_composite_events;
}

int32_t SensorBackendManager::FindHardwareSensorHandle(SensorType type) {
    for (size_t i = 0; i < backends_.size(); i++) {
        if (!backends_[i].initialized) {
            continue;
        }
        for (const auto& sensor_info : backends_[i].sensors) {
            if (sensor_info.type == type) {
                auto it = backends_[i].local_to_global_handles.find(sensor_info.sensorHandle);
                if (it != backends_[i].local_to_global_handles.end()) {
                    return it->second;
                }
            }
        }
    }
    return -1;
}

std::optional<int64_t> SensorBackendManager::EffectiveSamplingPeriodLocked(
        int32_t hardware_handle) {
    std::optional<int64_t> period;
    const auto direct = direct_sampling_period_ns_.find(hardware_handle);
    if (directly_activated_.count(hardware_handle) && direct != direct_sampling_period_ns_.end()) {
        period = direct->second;
    }

    for (size_t index = 0; index < composite_sensors_.size(); ++index) {
        if (!composite_sensors_[index]->IsActive()) continue;
        const auto request = composite_sampling_period_ns_.find(index);
        if (request == composite_sampling_period_ns_.end()) continue;
        for (SensorType type : composite_sensors_[index]->GetInputSensorTypes()) {
            if (FindHardwareSensorHandle(type) != hardware_handle) continue;
            period = period.has_value() ? std::min(*period, request->second) : request->second;
            break;
        }
    }
    return period;
}

void SensorBackendManager::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        post_events_callback_ = callback;
    }

    for (size_t i = 0; i < backends_.size(); i++) {
        auto& entry = backends_[i];

        auto wrapped_callback = [this, i](const std::vector<Event>& events, bool wakeup) {
            std::lock_guard<std::mutex> callback_lock(callback_mutex_);
            std::vector<Event> remapped_events;
            std::vector<Event> composite_events;
            PostEventsCallback callback;
            {
                std::lock_guard<std::mutex> cb_lock(mutex_);
                if (i >= backends_.size() || !backends_[i].initialized) {
                    return;
                }
                for (const auto& event : events) {
                    auto it = backends_[i].local_to_global_handles.find(event.sensorHandle);
                    if (it == backends_[i].local_to_global_handles.end()) {
                        LOG(WARNING)
                                << "Dropping event with unknown local handle=" << event.sensorHandle
                                << " from backend '" << backends_[i].name << "'";
                        continue;
                    }
                    Event remapped_event = event;
                    remapped_event.sensorHandle = it->second;
                    remapped_events.push_back(std::move(remapped_event));
                }
                composite_events = ProcessCompositeSensors(remapped_events);

                remapped_events.erase(
                        std::remove_if(remapped_events.begin(), remapped_events.end(),
                                       [this](const Event& ev) {
                                           return !directly_activated_.count(ev.sensorHandle);
                                       }),
                        remapped_events.end());
                callback = post_events_callback_;
            }

            if (!composite_events.empty()) {
                remapped_events.insert(remapped_events.end(), composite_events.begin(),
                                       composite_events.end());
            }

            if (remapped_events.empty()) {
                return;
            }

            if (callback) {
                callback(remapped_events, wakeup);
            }
        };

        int32_t result = entry.backend->Initialize(wrapped_callback);
        if (result != 0) {
            LOG(WARNING) << "Backend '" << entry.name << "' failed to initialize: " << result;
            entry.backend->Deinitialize();
            continue;
        }

        auto sensors = entry.backend->GetSensorsList();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entry.initialized = true;
            entry.sensors = std::move(sensors);
            for (auto& sensor_info : entry.sensors) {
                sensor_info.flags &=
                        ~static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
                int32_t local_handle = sensor_info.sensorHandle;
                int32_t global_handle = next_handle_++;

                entry.local_to_global_handles[local_handle] = global_handle;
                entry.global_to_local_handles[global_handle] = local_handle;
                global_handle_to_backend_[global_handle] = i;

                LOG(INFO) << "Sensor [backend='" << entry.name << "'] handle=" << global_handle
                          << " type=" << static_cast<int32_t>(sensor_info.type) << " name='"
                          << sensor_info.name << "' vendor='" << sensor_info.vendor << "'";
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    std::set<SensorType> hardware_sensor_types;
    for (const auto& entry : backends_) {
        if (!entry.initialized) {
            continue;
        }
        for (const auto& sensor_info : entry.sensors) {
            hardware_sensor_types.insert(sensor_info.type);
        }
    }

    std::vector<std::unique_ptr<ICompositeSensor>> active_composites;
    for (auto& composite : composite_sensors_) {
        auto info = composite->GetSensorInfo();
        if (hardware_sensor_types.count(info.type) > 0) {
            LOG(INFO) << "Composite sensor [type=" << static_cast<int32_t>(info.type) << " name='"
                      << info.name << "'] skipped: hardware sensor already provides this type";
            continue;
        }
        active_composites.push_back(std::move(composite));
    }
    composite_sensors_ = std::move(active_composites);

    for (size_t ci = 0; ci < composite_sensors_.size(); ci++) {
        auto& composite = composite_sensors_[ci];
        int32_t handle = next_handle_++;
        composite->SetHandle(handle);
        composite_handle_to_index_[handle] = ci;

        for (const auto& input_type : composite->GetInputSensorTypes()) {
            sensor_type_to_composite_[input_type].push_back(ci);
        }

        auto info = composite->GetSensorInfo();
        LOG(INFO) << "Composite sensor [type=" << static_cast<int32_t>(info.type) << " name='"
                  << info.name << "'] registered with handle=" << handle;
    }
}

void SensorBackendManager::Deinitialize() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::vector<ISensorBackend*> backends;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& composite : composite_sensors_) {
            composite->Activate(false);
        }
        hardware_dependency_count_.clear();
        directly_activated_.clear();
        direct_sampling_period_ns_.clear();
        composite_sampling_period_ns_.clear();
        global_handle_to_backend_.clear();
        composite_handle_to_index_.clear();
        sensor_type_to_composite_.clear();
        next_handle_ = 1;
        for (auto& entry : backends_) {
            if (entry.initialized) {
                backends.push_back(entry.backend.get());
            }
            entry.initialized = false;
            entry.sensors.clear();
            entry.local_to_global_handles.clear();
            entry.global_to_local_handles.clear();
        }
        post_events_callback_ = nullptr;
    }
    // Wait for a callback that already copied post_events_callback_ to finish.
    {
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    }
    for (auto* backend : backends) backend->Deinitialize();
}

std::vector<SensorInfo> SensorBackendManager::GetSensorsList() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> all_sensors;

    for (size_t i = 0; i < backends_.size(); i++) {
        if (!backends_[i].initialized) {
            continue;
        }
        for (auto sensor_info : backends_[i].sensors) {
            auto it = backends_[i].local_to_global_handles.find(sensor_info.sensorHandle);
            if (it != backends_[i].local_to_global_handles.end()) {
                sensor_info.sensorHandle = it->second;
                all_sensors.push_back(sensor_info);
            }
        }
    }

    for (auto& composite : composite_sensors_) {
        auto info = composite->GetSensorInfo();
        all_sensors.push_back(info);
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
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    struct BackendAction {
        ISensorBackend* backend;
        int32_t local_handle;
        bool enabled;
    };
    std::vector<BackendAction> actions;
    std::vector<int32_t> dependency_handles;
    bool is_composite = false;
    size_t composite_index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cs_it = composite_handle_to_index_.find(sensor_handle);
        if (cs_it != composite_handle_to_index_.end()) {
            is_composite = true;
            composite_index = cs_it->second;
            auto& composite = composite_sensors_[composite_index];
            if (composite->IsActive() == enabled) {
                return 0;
            }

            for (const auto& input_type : composite->GetInputSensorTypes()) {
                int32_t hw_handle = FindHardwareSensorHandle(input_type);
                if (hw_handle < 0) {
                    return -EINVAL;
                }
                const auto backend_it = global_handle_to_backend_.find(hw_handle);
                if (backend_it == global_handle_to_backend_.end()) {
                    return -EINVAL;
                }
                auto& entry = backends_[backend_it->second];
                const auto local_it = entry.global_to_local_handles.find(hw_handle);
                if (local_it == entry.global_to_local_handles.end()) {
                    return -EINVAL;
                }
                dependency_handles.push_back(hw_handle);
            }

            composite->Activate(enabled);
            for (int32_t hw_handle : dependency_handles) {
                auto& entry = backends_[global_handle_to_backend_.at(hw_handle)];
                const int32_t local_handle = entry.global_to_local_handles.at(hw_handle);
                int32_t& count = hardware_dependency_count_[hw_handle];
                const int32_t old_total = count + directly_activated_.count(hw_handle);

                if (enabled) {
                    ++count;
                } else {
                    --count;
                }
                const int32_t new_total = count + directly_activated_.count(hw_handle);
                if (old_total == 0 && new_total > 0) {
                    actions.push_back({entry.backend.get(), local_handle, true});
                } else if (old_total > 0 && new_total == 0) {
                    actions.push_back({entry.backend.get(), local_handle, false});
                }
            }
        }
    }
    if (is_composite) {
        size_t completed = 0;
        for (; completed < actions.size(); ++completed) {
            const auto& action = actions[completed];
            const int32_t result = action.backend->Activate(action.local_handle, action.enabled);
            if (result == 0) continue;
            while (completed > 0) {
                const auto& previous = actions[--completed];
                previous.backend->Activate(previous.local_handle, !previous.enabled);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            composite_sensors_[composite_index]->Activate(!enabled);
            for (int32_t handle : dependency_handles) {
                int32_t& count = hardware_dependency_count_[handle];
                count += enabled ? -1 : 1;
            }
            return result;
        }
        std::vector<std::tuple<ISensorBackend*, int32_t, int64_t>> batch_actions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int32_t handle : dependency_handles) {
                const auto period = EffectiveSamplingPeriodLocked(handle);
                if (!period.has_value()) continue;
                auto& entry = backends_[global_handle_to_backend_.at(handle)];
                batch_actions.emplace_back(entry.backend.get(),
                                           entry.global_to_local_handles.at(handle), *period);
            }
        }
        for (const auto& [backend, local_handle, period] : batch_actions) {
            const int32_t result = backend->Batch(local_handle, period, 0);
            if (result == 0) continue;
            if (!enabled) {
                LOG(WARNING) << "Failed to update dependency rate after composite deactivation: "
                             << result;
                continue;
            }
            for (auto action = actions.rbegin(); action != actions.rend(); ++action) {
                action->backend->Activate(action->local_handle, !action->enabled);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            composite_sensors_[composite_index]->Activate(false);
            for (int32_t handle : dependency_handles) {
                --hardware_dependency_count_[handle];
            }
            return result;
        }
        return 0;
    }

    ISensorBackend* backend = nullptr;
    int32_t local_handle = -1;
    bool call_backend = false;
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

        const bool directly_active = directly_activated_.count(sensor_handle) > 0;
        if (directly_active == enabled) {
            return 0;
        }
        const int32_t old_total = hardware_dependency_count_[sensor_handle] + directly_active;
        if (enabled) {
            directly_activated_.insert(sensor_handle);
        } else {
            directly_activated_.erase(sensor_handle);
        }
        const int32_t new_total = hardware_dependency_count_[sensor_handle] + enabled;
        call_backend = (old_total == 0) != (new_total == 0);
    }
    int32_t result = call_backend ? backend->Activate(local_handle, enabled) : 0;
    if (result != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled)
            directly_activated_.erase(sensor_handle);
        else
            directly_activated_.insert(sensor_handle);
        return result;
    }
    std::optional<int64_t> period;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        period = EffectiveSamplingPeriodLocked(sensor_handle);
    }
    if (period.has_value()) result = backend->Batch(local_handle, *period, 0);
    return result;
}

int32_t SensorBackendManager::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                                    int64_t max_report_latency_ns) {
    if (sampling_period_ns < 0 || max_report_latency_ns < 0) {
        return -EINVAL;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    struct BatchAction {
        ISensorBackend* backend;
        int32_t local_handle;
        int64_t period;
    };
    std::vector<BatchAction> composite_actions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cs_it = composite_handle_to_index_.find(sensor_handle);
        if (cs_it != composite_handle_to_index_.end()) {
            composite_sampling_period_ns_[cs_it->second] = sampling_period_ns;
            if (!composite_sensors_[cs_it->second]->IsActive()) return 0;
            for (const auto& input_type :
                 composite_sensors_[cs_it->second]->GetInputSensorTypes()) {
                const int32_t hw_handle = FindHardwareSensorHandle(input_type);
                if (hw_handle < 0) return -EINVAL;
                const auto backend_it = global_handle_to_backend_.find(hw_handle);
                if (backend_it == global_handle_to_backend_.end()) return -EINVAL;
                auto& entry = backends_[backend_it->second];
                auto local_it = entry.global_to_local_handles.find(hw_handle);
                if (local_it == entry.global_to_local_handles.end()) return -EINVAL;
                const auto period = EffectiveSamplingPeriodLocked(hw_handle);
                composite_actions.push_back({entry.backend.get(), local_it->second,
                                             period.value_or(sampling_period_ns)});
            }
        }
    }
    if (!composite_actions.empty()) {
        for (const auto& action : composite_actions) {
            const int32_t result = action.backend->Batch(action.local_handle, action.period, 0);
            if (result != 0) return result;
        }
        return 0;
    }

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
        direct_sampling_period_ns_[sensor_handle] = sampling_period_ns;
        const auto effective = EffectiveSamplingPeriodLocked(sensor_handle);
        if (effective.has_value()) sampling_period_ns = *effective;
    }
    return backend->Batch(local_handle, sampling_period_ns, max_report_latency_ns);
}

int32_t SensorBackendManager::Flush(int32_t sensor_handle) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    PostEventsCallback callback;
    Event flush_event;
    bool is_composite = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto cs_it = composite_handle_to_index_.find(sensor_handle);
        if (cs_it != composite_handle_to_index_.end()) {
            size_t ci = cs_it->second;
            if (!composite_sensors_[ci]->IsActive()) {
                return -EINVAL;
            }

            flush_event = composite_sensors_[ci]->CreateFlushCompleteEvent();
            callback = post_events_callback_;
            is_composite = true;
        }
    }
    if (is_composite) {
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
        if (callback) callback({flush_event}, false);
        return 0;
    }

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
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::vector<ISensorBackend*> backends;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : backends_) {
            if (entry.initialized) {
                backends.push_back(entry.backend.get());
            }
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
