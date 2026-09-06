/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsManager"

#include "SensorManager.h"

#include <android-base/logging.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/SensorTypes.h>
#include <libsensors_common/Settings.h>

#include <algorithm>
#include <cerrno>

namespace aidl::android::hardware::sensors::mainline {

namespace {
// Rate used when the framework activates a sensor without calling batch()
// first.
constexpr int64_t kDefaultPeriodNs = 66666667;  // 15 Hz
}  // namespace

SensorManager::SensorManager() = default;

SensorManager::~SensorManager() {
    DeactivateAll();
    std::lock_guard<std::mutex> lock(control_mutex_);
    for (auto& entry : backends_) {
        if (entry.initialized && entry.loaded.backend) {
            entry.loaded.backend->Deinitialize();
        }
    }
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        sink_ = nullptr;
        hardware_.clear();
        composites_.clear();
    }
    backends_.clear();
}

void SensorManager::RegisterCompositeSensor(std::unique_ptr<ICompositeSensor> sensor) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (initialized_) {
        LOG(ERROR) << "Composite sensors must be registered before Initialize()";
        return;
    }
    pending_composites_.push_back(std::move(sensor));
}

void SensorManager::Initialize() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (initialized_) {
        return;
    }
    initialized_ = true;
    log_events_ = Settings::Get().GetBool("debug.log_events", false);

    std::vector<LoadedBackend> loaded =
            BackendLoader::LoadBackends(BackendLoader::ResolveBackendList());
    for (auto& backend : loaded) {
        BackendEntry entry;
        entry.loaded = std::move(backend);
        backends_.push_back(std::move(entry));
    }

    for (size_t i = 0; i < backends_.size(); i++) {
        BackendEntry& entry = backends_[i];
        ISensorBackend* backend = entry.loaded.backend.get();
        PostEventsCallback callback = [this, i](const std::vector<Event>& events, bool wakeup) {
            OnBackendEvents(i, events, wakeup);
        };
        int32_t ret = backend->Initialize(callback);
        if (ret != 0) {
            LOG(ERROR) << "Backend '" << backend->GetName() << "' failed to initialize: " << ret;
            continue;
        }
        entry.initialized = true;
    }

    RegisterHardwareSensors();
    RegisterComposites();

    LOG(INFO) << "Sensor manager initialized: " << backends_.size() << " backend(s), "
              << hardware_.size() << " hardware sensor(s), " << composites_.size()
              << " composite sensor(s)";
}

void SensorManager::RegisterHardwareSensors() {
    std::lock_guard<std::mutex> state(state_mutex_);
    std::set<SensorType> provided_types;
    std::set<std::pair<SensorType, std::string>> used_names;

    for (size_t i = 0; i < backends_.size(); i++) {
        BackendEntry& entry = backends_[i];
        if (!entry.initialized) {
            continue;
        }
        ISensorBackend* backend = entry.loaded.backend.get();
        const std::string backend_name = backend->GetName();
        const bool fallback_only = (entry.loaded.flags & kSensorBackendFlagFallbackOnly) != 0;
        std::set<SensorType> backend_types;

        for (SensorInfo info : backend->GetSensorsList()) {
            const int32_t local_handle = info.sensorHandle;
            if (fallback_only && provided_types.count(info.type) != 0) {
                LOG(INFO) << "Sensor [" << backend_name
                          << "] skipped, type already provided: " << SensorInfoToString(info);
                continue;
            }
            if (entry.local_to_global.count(local_handle) != 0) {
                LOG(ERROR) << "Sensor [" << backend_name << "] duplicate local handle "
                           << local_handle << ", skipped: " << SensorInfoToString(info);
                continue;
            }
            if (info.type == SensorType::META_DATA || info.type == SensorType::ADDITIONAL_INFO ||
                info.name.empty()) {
                LOG(ERROR) << "Sensor [" << backend_name
                           << "] invalid, skipped: " << SensorInfoToString(info);
                continue;
            }
            if (info.vendor.empty()) {
                info.vendor = backend_name;
            }
            // Names must be unique per type across backends.
            if (used_names.count({info.type, info.name}) != 0) {
                std::string renamed = info.name + " (" + backend_name + ")";
                LOG(INFO) << "Sensor [" << backend_name << "] renamed '" << info.name << "' -> '"
                          << renamed << "' to keep names unique";
                info.name = renamed;
            }
            used_names.insert({info.type, info.name});

            HardwareSensor sensor;
            sensor.handle = next_handle_++;
            sensor.backend_index = i;
            sensor.local_handle = local_handle;
            info.sensorHandle = sensor.handle;
            sensor.info = info;
            sensor.client_period_ns = ClampSamplingPeriodNs(info, kDefaultPeriodNs);
            backend_types.insert(info.type);
            entry.local_to_global[local_handle] = sensor.handle;
            LOG(INFO) << "Sensor [" << backend_name << "] registered: " << SensorInfoToString(info)
                      << " (local handle " << local_handle << ")";
            hardware_[sensor.handle] = std::move(sensor);
        }
        provided_types.insert(backend_types.begin(), backend_types.end());
    }
}

void SensorManager::RegisterComposites() {
    std::lock_guard<std::mutex> state(state_mutex_);
    for (auto& composite : pending_composites_) {
        SensorInfo info = composite->GetSensorInfo();
        bool type_provided =
                std::any_of(hardware_.begin(), hardware_.end(),
                            [&](const auto& entry) { return entry.second.info.type == info.type; });
        if (type_provided) {
            LOG(INFO) << "Composite sensor '" << info.name
                      << "' skipped: a hardware sensor of type " << toString(info.type)
                      << " exists";
            continue;
        }

        std::vector<int32_t> inputs;
        bool inputs_available = true;
        for (SensorType input_type : composite->GetInputSensorTypes()) {
            auto it = std::find_if(hardware_.begin(), hardware_.end(), [&](const auto& entry) {
                return entry.second.info.type == input_type;
            });
            if (it == hardware_.end()) {
                LOG(INFO) << "Composite sensor '" << info.name << "' skipped: no "
                          << toString(input_type) << " available";
                inputs_available = false;
                break;
            }
            inputs.push_back(it->first);
        }
        if (!inputs_available) {
            continue;
        }

        CompositeEntry entry;
        entry.handle = next_handle_++;
        composite->SetHandle(entry.handle);
        entry.sensor = std::move(composite);
        entry.input_handles = inputs;
        for (int32_t input : inputs) {
            hardware_[input].composite_subscribers.push_back(entry.handle);
        }
        LOG(INFO) << "Composite sensor registered: "
                  << SensorInfoToString(entry.sensor->GetSensorInfo());
        composites_.push_back(std::move(entry));
    }
    pending_composites_.clear();
}

void SensorManager::SetEventSink(PostEventsCallback sink) {
    std::lock_guard<std::mutex> state(state_mutex_);
    sink_ = std::move(sink);
}

void SensorManager::Deliver(const std::vector<Event>& events, bool wakeup) {
    if (events.empty()) {
        return;
    }
    PostEventsCallback sink;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        sink = sink_;
    }
    if (sink) {
        sink(events, wakeup);
    }
}

void SensorManager::OnBackendEvents(size_t backend_index, const std::vector<Event>& events,
                                    bool wakeup) {
    std::vector<Event> out;
    bool wake = false;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        if (backend_index >= backends_.size()) {
            return;
        }
        if (mode_ == OperationMode::DATA_INJECTION) {
            // Real data is replaced by injected data in this mode.
            return;
        }
        const auto& mapping = backends_[backend_index].local_to_global;
        for (Event event : events) {
            auto it = mapping.find(event.sensorHandle);
            if (it == mapping.end()) {
                LOG(WARNING) << "Event from backend "
                             << backends_[backend_index].loaded.backend->GetName()
                             << " for unknown local handle " << event.sensorHandle;
                continue;
            }
            event.sensorHandle = it->second;
            HardwareSensor* hw = FindHardware(event.sensorHandle);
            if (hw == nullptr) {
                continue;
            }
            if (log_events_) {
                LOG(INFO) << "Event: " << EventToString(event);
            }

            const bool is_meta = event.sensorType == SensorType::META_DATA;
            if (!is_meta) {
                for (int32_t composite_handle : hw->composite_subscribers) {
                    CompositeEntry* composite = FindComposite(composite_handle);
                    if (composite == nullptr || !composite->sensor->IsActive()) {
                        continue;
                    }
                    std::vector<Event> derived = composite->sensor->ProcessEvent(event);
                    for (const Event& d : derived) {
                        out.push_back(d);
                        wake = wake || IsWakeUpSensor(composite->sensor->GetSensorInfo().flags);
                    }
                }
            }
            if (hw->client_active) {
                out.push_back(event);
                wake = wake || wakeup || IsWakeUpSensor(hw->info.flags);
            }
        }
    }
    Deliver(out, wake);
}

std::vector<SensorInfo> SensorManager::GetSensorsList() const {
    std::lock_guard<std::mutex> state(state_mutex_);
    std::vector<SensorInfo> list;
    for (const auto& [handle, sensor] : hardware_) {
        list.push_back(sensor.info);
    }
    for (const auto& composite : composites_) {
        list.push_back(composite.sensor->GetSensorInfo());
    }
    return list;
}

std::optional<SensorInfo> SensorManager::GetSensorInfo(int32_t handle) const {
    std::lock_guard<std::mutex> state(state_mutex_);
    const HardwareSensor* hw = FindHardware(handle);
    if (hw != nullptr) {
        return hw->info;
    }
    for (const auto& composite : composites_) {
        if (composite.handle == handle) {
            return composite.sensor->GetSensorInfo();
        }
    }
    return std::nullopt;
}

SensorManager::HardwareSensor* SensorManager::FindHardware(int32_t handle) {
    auto it = hardware_.find(handle);
    return it == hardware_.end() ? nullptr : &it->second;
}

const SensorManager::HardwareSensor* SensorManager::FindHardware(int32_t handle) const {
    auto it = hardware_.find(handle);
    return it == hardware_.end() ? nullptr : &it->second;
}

SensorManager::CompositeEntry* SensorManager::FindComposite(int32_t handle) {
    for (auto& composite : composites_) {
        if (composite.handle == handle) {
            return &composite;
        }
    }
    return nullptr;
}

// Brings the backend state of a hardware sensor in line with the requests of
// the framework and of the composite sensors. control_mutex_ must be held,
// state_mutex_ must not.
int32_t SensorManager::ApplyHardwareState(int32_t handle) {
    ISensorBackend* backend = nullptr;
    int32_t local_handle = 0;
    bool desired_active = false;
    int64_t desired_period = 0;
    int64_t desired_latency = 0;
    bool need_batch = false;
    bool need_activate = false;
    std::string name;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        HardwareSensor* hw = FindHardware(handle);
        if (hw == nullptr) {
            return -EINVAL;
        }
        backend = backends_[hw->backend_index].loaded.backend.get();
        local_handle = hw->local_handle;
        name = hw->info.name;

        desired_active = hw->client_active || !hw->composite_periods.empty();
        desired_period = hw->client_active ? hw->client_period_ns : 0;
        for (const auto& [composite, period] : hw->composite_periods) {
            int64_t p = ClampSamplingPeriodNs(hw->info, period);
            desired_period = desired_period == 0 ? p : std::min(desired_period, p);
        }
        if (desired_period == 0) {
            desired_period = hw->client_period_ns;
        }
        desired_latency = hw->client_active ? hw->client_latency_ns : 0;

        need_batch = desired_active && (desired_period != hw->backend_period_ns ||
                                        desired_latency != hw->backend_latency_ns);
        need_activate = desired_active != hw->backend_active;
    }

    int32_t ret = 0;
    if (need_batch) {
        ret = backend->Batch(local_handle, desired_period, desired_latency);
        if (ret != 0) {
            LOG(WARNING) << "Batch failed for '" << name << "': " << ret;
        } else {
            std::lock_guard<std::mutex> state(state_mutex_);
            if (HardwareSensor* hw = FindHardware(handle)) {
                hw->backend_period_ns = desired_period;
                hw->backend_latency_ns = desired_latency;
            }
        }
    }
    if (need_activate) {
        LOG(INFO) << (desired_active ? "Activating" : "Deactivating") << " '" << name
                  << "' in backend " << backend->GetName() << " (period " << desired_period / 1000
                  << " us)";
        ret = backend->Activate(local_handle, desired_active);
        if (ret != 0) {
            LOG(ERROR) << "Activate(" << desired_active << ") failed for '" << name << "': " << ret;
        } else {
            std::lock_guard<std::mutex> state(state_mutex_);
            if (HardwareSensor* hw = FindHardware(handle)) {
                hw->backend_active = desired_active;
            }
        }
    }
    return ret;
}

int32_t SensorManager::ActivateComposite(CompositeEntry* composite, bool enabled) {
    std::vector<int32_t> inputs;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        if (composite->sensor->IsActive() == enabled) {
            return 0;
        }
        composite->sensor->Activate(enabled);
        inputs = composite->input_handles;
        for (int32_t input : inputs) {
            HardwareSensor* hw = FindHardware(input);
            if (hw == nullptr) continue;
            if (enabled) {
                hw->composite_periods[composite->handle] =
                        composite->sensor->GetInputSamplingPeriodNs();
            } else {
                hw->composite_periods.erase(composite->handle);
            }
        }
    }
    int32_t ret = 0;
    for (int32_t input : inputs) {
        int32_t r = ApplyHardwareState(input);
        if (r != 0) ret = r;
    }
    return ret;
}

int32_t SensorManager::Activate(int32_t handle, bool enabled) {
    std::lock_guard<std::mutex> control(control_mutex_);
    if (CompositeEntry* composite = FindComposite(handle)) {
        return ActivateComposite(composite, enabled);
    }
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        HardwareSensor* hw = FindHardware(handle);
        if (hw == nullptr) {
            return -EINVAL;
        }
        if (hw->client_active == enabled) {
            return 0;
        }
        hw->client_active = enabled;
    }
    int32_t ret = ApplyHardwareState(handle);
    if (ret != 0 && enabled) {
        std::lock_guard<std::mutex> state(state_mutex_);
        if (HardwareSensor* hw = FindHardware(handle)) {
            hw->client_active = false;
        }
    }
    return ret;
}

int32_t SensorManager::Batch(int32_t handle, int64_t sampling_period_ns,
                             int64_t max_report_latency_ns) {
    std::lock_guard<std::mutex> control(control_mutex_);
    if (CompositeEntry* composite = FindComposite(handle)) {
        std::lock_guard<std::mutex> state(state_mutex_);
        composite->sensor->Batch(sampling_period_ns);
        return 0;
    }
    bool apply = false;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        HardwareSensor* hw = FindHardware(handle);
        if (hw == nullptr) {
            return -EINVAL;
        }
        if (sampling_period_ns < 0 || max_report_latency_ns < 0) {
            return -EINVAL;
        }
        hw->client_period_ns = ClampSamplingPeriodNs(hw->info, sampling_period_ns);
        hw->client_latency_ns = max_report_latency_ns;
        apply = hw->client_active;
        LOG(DEBUG) << "Batch '" << hw->info.name << "': requested " << sampling_period_ns / 1000
                   << " us -> " << hw->client_period_ns / 1000 << " us, latency "
                   << max_report_latency_ns / 1000 << " us";
    }
    return apply ? ApplyHardwareState(handle) : 0;
}

int32_t SensorManager::Flush(int32_t handle) {
    std::lock_guard<std::mutex> control(control_mutex_);
    if (CompositeEntry* composite = FindComposite(handle)) {
        bool active;
        {
            std::lock_guard<std::mutex> state(state_mutex_);
            active = composite->sensor->IsActive();
        }
        if (!active) {
            return -EINVAL;
        }
        Deliver({MakeFlushCompleteEvent(handle)},
                IsWakeUpSensor(composite->sensor->GetSensorInfo().flags));
        return 0;
    }

    ISensorBackend* backend = nullptr;
    int32_t local_handle = 0;
    bool wakeup = false;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        HardwareSensor* hw = FindHardware(handle);
        if (hw == nullptr) {
            return -EINVAL;
        }
        if (!hw->client_active || IsOneShotSensor(hw->info.flags)) {
            LOG(DEBUG) << "Flush rejected for '" << hw->info.name
                       << "': inactive or one-shot sensor";
            return -EINVAL;
        }
        backend = backends_[hw->backend_index].loaded.backend.get();
        local_handle = hw->local_handle;
        wakeup = IsWakeUpSensor(hw->info.flags);
    }
    int32_t ret = backend->Flush(local_handle);
    if (ret == kFlushHandledByFrontend) {
        Deliver({MakeFlushCompleteEvent(handle)}, wakeup);
        return 0;
    }
    return ret;
}

bool SensorManager::SupportsDataInjection() const {
    std::lock_guard<std::mutex> state(state_mutex_);
    return std::any_of(hardware_.begin(), hardware_.end(), [](const auto& entry) {
        return (entry.second.info.flags & SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION) != 0;
    });
}

int32_t SensorManager::SetOperationMode(OperationMode mode) {
    std::lock_guard<std::mutex> control(control_mutex_);
    if (mode == OperationMode::DATA_INJECTION && !SupportsDataInjection()) {
        LOG(INFO) << "Data injection mode requested but no sensor supports it";
        return -ENOTSUP;
    }
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        mode_ = mode;
    }
    LOG(INFO) << "Operation mode: " << toString(mode);
    int32_t ret = 0;
    for (auto& entry : backends_) {
        if (!entry.initialized) continue;
        int32_t r = entry.loaded.backend->SetOperationMode(mode);
        if (r != 0) {
            LOG(WARNING) << "Backend '" << entry.loaded.backend->GetName()
                         << "' rejected operation mode: " << r;
            ret = r;
        }
    }
    return ret;
}

int32_t SensorManager::InjectEvent(const Event& event) {
    if (event.sensorType == SensorType::ADDITIONAL_INFO) {
        // Operation environment parameters: accepted and ignored.
        return 0;
    }
    bool wakeup = false;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        const HardwareSensor* hw = FindHardware(event.sensorHandle);
        if (hw == nullptr) {
            return -EINVAL;
        }
        if ((hw->info.flags & SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION) == 0) {
            return -ENOTSUP;
        }
        if (mode_ != OperationMode::DATA_INJECTION) {
            return -EINVAL;
        }
        wakeup = IsWakeUpSensor(hw->info.flags);
    }
    Deliver({event}, wakeup);
    return 0;
}

void SensorManager::DeactivateAll() {
    std::lock_guard<std::mutex> control(control_mutex_);
    std::vector<int32_t> handles;
    {
        std::lock_guard<std::mutex> state(state_mutex_);
        for (auto& composite : composites_) {
            if (composite.sensor->IsActive()) {
                composite.sensor->Activate(false);
            }
        }
        for (auto& [handle, hw] : hardware_) {
            hw.client_active = false;
            hw.composite_periods.clear();
            if (hw.backend_active) {
                handles.push_back(handle);
            }
        }
    }
    for (int32_t handle : handles) {
        ApplyHardwareState(handle);
    }
    LOG(INFO) << "All sensors deactivated (" << handles.size() << " were active)";
}

}  // namespace aidl::android::hardware::sensors::mainline
