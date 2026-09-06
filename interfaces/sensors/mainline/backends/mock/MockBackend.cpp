/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsMock"

#include "MockBackend.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/SensorTypes.h>
#include <libsensors_common/Settings.h>

#include <cerrno>
#include <cmath>

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr int64_t kDefaultPeriodNs = 200LL * 1000 * 1000;
constexpr float kGravity = 9.80665f;
constexpr float kTwoPi = 6.28318530718f;

const SensorType kDefaultSensorTypes[] = {
        SensorType::ACCELEROMETER,       SensorType::GYROSCOPE,
        SensorType::MAGNETIC_FIELD,      SensorType::LIGHT,
        SensorType::PROXIMITY,           SensorType::PRESSURE,
        SensorType::AMBIENT_TEMPERATURE, SensorType::RELATIVE_HUMIDITY,
};

}  // namespace

DEFINE_SENSOR_BACKEND(MockBackend, kSensorBackendFlagFallbackOnly)

MockBackend::MockBackend() = default;

MockBackend::~MockBackend() {
    Deinitialize();
}

std::string MockBackend::GetName() const {
    return "mock";
}

int32_t MockBackend::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    post_events_ = callback;

    std::string configured = Settings::Get().GetString("mock.sensors", "");
    if (configured.empty()) {
        for (SensorType type : kDefaultSensorTypes) {
            CreateSensor(type);
        }
    } else if (!::android::base::EqualsIgnoreCase(configured, "none")) {
        for (const auto& token : ::android::base::Split(configured, ",")) {
            std::string name = ::android::base::Trim(token);
            if (name.empty()) {
                continue;
            }
            auto type = ParseSensorType(name);
            if (!type.has_value()) {
                LOG(WARNING) << "mock.sensors: unknown sensor type '" << name << "'";
                continue;
            }
            CreateSensor(*type);
        }
    }

    LOG(INFO) << "Mock backend initialized with " << sensors_.size() << " sensor(s)";
    return 0;
}

void MockBackend::CreateSensor(SensorType type) {
    auto traits = GetSensorTypeTraits(type);
    if (!traits.has_value()) {
        LOG(WARNING) << "No traits for sensor type " << toString(type) << ", not mocked";
        return;
    }

    auto sensor = std::make_unique<MockSensor>();
    sensor->info.sensorHandle = next_handle_++;
    sensor->info.name = std::string("Mock ") + traits->label;
    sensor->info.vendor = "Mainline Sensors HAL";
    sensor->info.version = 1;
    sensor->info.type = type;
    ApplySensorTypeDefaults(&sensor->info);
    // Fake data can be replaced by injected data for testing.
    if (traits->is_vec3) {
        sensor->info.flags |= SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION;
    }
    sensor->period_ns = kDefaultPeriodNs;

    LOG(INFO) << "Mock sensor created: " << SensorInfoToString(sensor->info);
    int32_t handle = sensor->info.sensorHandle;
    sensors_[handle] = std::move(sensor);
}

void MockBackend::Deinitialize() {
    std::vector<std::unique_ptr<PeriodicWorker>> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [handle, sensor] : sensors_) {
            sensor->active = false;
            if (sensor->worker) {
                workers.push_back(std::move(sensor->worker));
            }
        }
        post_events_ = nullptr;
    }
    // Workers may be blocked on mutex_ inside GenerateSample(); stop them
    // without holding it.
    for (auto& worker : workers) {
        worker->Stop();
    }
    {
        // Leave a clean slate: Initialize() may be called again to retry
        // discovery while the frontend waits for late sensors.
        std::lock_guard<std::mutex> lock(mutex_);
        sensors_.clear();
        next_handle_ = 1;
    }
    LOG(INFO) << "Mock backend deinitialized";
}

std::vector<SensorInfo> MockBackend::GetSensorsList() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> list;
    for (const auto& [handle, sensor] : sensors_) {
        list.push_back(sensor->info);
    }
    return list;
}

Event MockBackend::BuildEvent(MockSensor* sensor, int64_t timestamp_ns) {
    const int32_t handle = sensor->info.sensorHandle;
    // Slow oscillation so that the data visibly changes over time.
    const float phase = static_cast<float>(sensor->sample_count % 1000) / 1000.0f * kTwoPi;
    const float wobble = std::sin(phase);
    // Toggle every ~5 seconds for on-change sensors.
    const bool toggle = ((sensor->sample_count * static_cast<uint64_t>(sensor->period_ns)) /
                         (5ULL * 1000 * 1000 * 1000)) %
                                2 ==
                        1;

    switch (sensor->info.type) {
        case SensorType::ACCELEROMETER:
            return MakeVec3Event(handle, sensor->info.type, timestamp_ns, 0.05f * wobble,
                                 0.05f * wobble, kGravity);
        case SensorType::GYROSCOPE:
            return MakeVec3Event(handle, sensor->info.type, timestamp_ns, 0.01f * wobble, 0.0f,
                                 0.0f);
        case SensorType::MAGNETIC_FIELD:
            return MakeVec3Event(handle, sensor->info.type, timestamp_ns, 20.0f + wobble, 0.0f,
                                 -40.0f);
        case SensorType::LIGHT:
            return MakeScalarEvent(handle, sensor->info.type, timestamp_ns,
                                   toggle ? 400.0f : 80.0f);
        case SensorType::PROXIMITY:
            return MakeScalarEvent(handle, sensor->info.type, timestamp_ns, toggle ? 0.0f : 5.0f);
        case SensorType::PRESSURE:
            return MakeScalarEvent(handle, sensor->info.type, timestamp_ns,
                                   1013.25f + 0.5f * wobble);
        case SensorType::AMBIENT_TEMPERATURE:
            return MakeScalarEvent(handle, sensor->info.type, timestamp_ns, toggle ? 25.5f : 25.0f);
        case SensorType::RELATIVE_HUMIDITY:
            return MakeScalarEvent(handle, sensor->info.type, timestamp_ns, toggle ? 51.0f : 50.0f);
        case SensorType::STEP_COUNTER:
            return MakeStepCountEvent(handle, timestamp_ns,
                                      static_cast<int64_t>(sensor->sample_count));
        default:
            return MakeScalarEvent(handle, sensor->info.type, timestamp_ns, toggle ? 1.0f : 0.0f);
    }
}

void MockBackend::GenerateSample(MockSensor* sensor) {
    PostEventsCallback callback;
    std::optional<Event> event;
    bool wakeup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sensor->active || operation_mode_.load() != OperationMode::NORMAL) {
            return;
        }
        Event candidate = BuildEvent(sensor, GetBootTimeNs());
        sensor->sample_count++;
        if (IsOnChangeSensor(sensor->info.flags) && sensor->last_event.has_value() &&
            HaveSamePayload(*sensor->last_event, candidate)) {
            return;
        }
        sensor->last_event = candidate;
        event = candidate;
        wakeup = IsWakeUpSensor(sensor->info.flags);
        callback = post_events_;
    }
    if (callback && event.has_value()) {
        LOG(VERBOSE) << "Mock event: " << EventToString(*event);
        callback({*event}, wakeup);
    }
}

int32_t MockBackend::Activate(int32_t sensor_handle, bool enabled) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = sensors_.find(sensor_handle);
    if (it == sensors_.end()) {
        return -EINVAL;
    }
    MockSensor* sensor = it->second.get();
    if (sensor->active == enabled) {
        return 0;
    }
    sensor->active = enabled;
    sensor->last_event.reset();
    if (enabled) {
        if (!sensor->worker) {
            sensor->worker =
                    std::make_unique<PeriodicWorker>("mock-" + std::to_string(sensor_handle),
                                                     [this, sensor]() { GenerateSample(sensor); });
        }
        sensor->worker->Start(sensor->period_ns);
    } else if (sensor->worker) {
        // Stop without holding the lock: the worker may be inside
        // GenerateSample() waiting for it.
        std::unique_ptr<PeriodicWorker> worker = std::move(sensor->worker);
        lock.unlock();
        worker->Stop();
        lock.lock();
    }
    LOG(INFO) << "Mock sensor " << sensor_handle << (enabled ? " activated" : " deactivated");
    return 0;
}

int32_t MockBackend::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                           int64_t /* max_report_latency_ns */) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sensors_.find(sensor_handle);
    if (it == sensors_.end()) {
        return -EINVAL;
    }
    MockSensor* sensor = it->second.get();
    if (sampling_period_ns <= 0) {
        sampling_period_ns = kDefaultPeriodNs;
    }
    sensor->period_ns = sampling_period_ns;
    if (sensor->worker) {
        sensor->worker->SetPeriod(sampling_period_ns);
    }
    LOG(DEBUG) << "Mock sensor " << sensor_handle << " period " << sampling_period_ns / 1000
               << " us";
    return 0;
}

int32_t MockBackend::Flush(int32_t sensor_handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sensors_.find(sensor_handle) == sensors_.end()) {
        return -EINVAL;
    }
    return kFlushHandledByFrontend;
}

int32_t MockBackend::SetOperationMode(OperationMode mode) {
    operation_mode_.store(mode);
    LOG(INFO) << "Mock backend operation mode " << toString(mode);
    return 0;
}

}  // namespace aidl::android::hardware::sensors::mainline
