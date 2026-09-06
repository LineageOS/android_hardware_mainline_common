/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsMockBackend"

#include "MockBackend.h"

#include <android-base/logging.h>

#include <cerrno>
#include <chrono>
#include <cmath>

namespace aidl::android::hardware::sensors::mainline {

static constexpr int32_t kDefaultMaxDelayUs = 10 * 1000 * 1000;
static constexpr int64_t kNanosecondsPerSecond = 1000LL * 1000 * 1000;

extern "C" __attribute__((visibility("default"))) ISensorBackend* CreateSensorBackend() {
    return new MockBackend();
}

MockBackend::MockBackend() = default;

MockBackend::~MockBackend() {
    Deinitialize();
}

std::string MockBackend::GetName() const {
    return "Mock";
}

void MockBackend::CreateMockSensors() {
    auto add_sensor = [this](SensorType type, const std::string& name, float max_range,
                             float resolution, float power, int32_t min_delay_us, int32_t flags) {
        auto sensor = std::make_unique<MockSensorData>();
        sensor->handle = next_handle_++;
        sensor->type = type;
        sensor->enabled = false;
        sensor->sampling_period_ns = 200 * 1000 * 1000;
        sensor->stop_thread = false;

        sensor->sensor_info.sensorHandle = sensor->handle;
        sensor->sensor_info.name = name;
        sensor->sensor_info.vendor = "Mock";
        sensor->sensor_info.version = 1;
        sensor->sensor_info.type = type;
        sensor->sensor_info.typeAsString = "";
        sensor->sensor_info.maxRange = max_range;
        sensor->sensor_info.resolution = resolution;
        sensor->sensor_info.power = power;
        sensor->sensor_info.minDelayUs = min_delay_us;
        sensor->sensor_info.maxDelayUs = kDefaultMaxDelayUs;
        sensor->sensor_info.fifoReservedEventCount = 0;
        sensor->sensor_info.fifoMaxEventCount = 0;
        sensor->sensor_info.requiredPermission = "";
        sensor->sensor_info.flags = flags;

        int32_t handle = sensor->handle;
        sensors_[handle] = std::move(sensor);
        LOG(INFO) << "Mock sensor created: " << name << " (handle=" << handle << ")";
    };

    add_sensor(SensorType::ACCELEROMETER, "Mock Accelerometer", 78.4f, 0.001f, 0.13f, 10000,
               static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION));

    add_sensor(SensorType::GYROSCOPE, "Mock Gyroscope", 1000.0f * static_cast<float>(M_PI) / 180.0f,
               1000.0f * static_cast<float>(M_PI) / (180.0f * 32768.0f), 6.1f, 10000,
               static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION));

    add_sensor(SensorType::MAGNETIC_FIELD, "Mock Magnetometer", 1300.0f, 0.01f, 0.35f, 10000,
               static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION));

    add_sensor(SensorType::LIGHT, "Mock Light Sensor", 43000.0f, 10.0f, 0.175f, 0,
               static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE));

    add_sensor(SensorType::PROXIMITY, "Mock Proximity Sensor", 5.0f, 1.0f, 0.012f, 0,
               static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE |
                                    SensorInfo::SENSOR_FLAG_BITS_WAKE_UP));

    add_sensor(SensorType::PRESSURE, "Mock Pressure Sensor", 1100.0f, 0.005f, 0.004f, 10000, 0);

    add_sensor(SensorType::AMBIENT_TEMPERATURE, "Mock Temperature Sensor", 80.0f, 0.01f, 0.001f, 0,
               static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE));

    add_sensor(SensorType::RELATIVE_HUMIDITY, "Mock Humidity Sensor", 100.0f, 0.1f, 0.001f, 0,
               static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE));
}

int32_t MockBackend::Initialize(const PostEventsCallback& callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    post_events_callback_ = callback;

    CreateMockSensors();

    LOG(INFO) << "Mock backend initialized with " << sensors_.size() << " sensor(s)";
    return 0;
}

void MockBackend::Deinitialize() {
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
    LOG(INFO) << "Mock backend deinitialized";
}

std::vector<SensorInfo> MockBackend::GetSensorsList() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> result;
    for (const auto& [handle, sensor] : sensors_) {
        result.push_back(sensor->sensor_info);
    }
    return result;
}

void MockBackend::PollSensorThread(MockSensorData* sensor) {
    LOG(DEBUG) << "Mock poll thread started for sensor " << sensor->handle;

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

        auto events = GenerateSensorData(sensor);
        if (!events.empty() && post_events_callback_) {
            bool wakeup = (sensor->sensor_info.flags &
                           static_cast<int32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP)) != 0;
            post_events_callback_(events, wakeup);
        }

        sensor->poll_cv.wait_for(lock, std::chrono::nanoseconds(period_ns));
    }

    LOG(DEBUG) << "Mock poll thread stopped for sensor " << sensor->handle;
}

std::vector<Event> MockBackend::GenerateSensorData(MockSensorData* sensor) {
    std::vector<Event> events;

    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    int64_t timestamp = ts.tv_sec * kNanosecondsPerSecond + ts.tv_nsec;

    Event event;
    event.sensorHandle = sensor->handle;
    event.sensorType = sensor->type;
    event.timestamp = timestamp;

    switch (sensor->type) {
        case SensorType::ACCELEROMETER: {
            EventPayload::Vec3 vec3 = {
                    .x = 0.0f,
                    .y = 0.0f,
                    .z = 9.8f,
                    .status = SensorStatus::ACCURACY_HIGH,
            };
            event.payload.set<EventPayload::Tag::vec3>(vec3);
            break;
        }
        case SensorType::GYROSCOPE: {
            EventPayload::Vec3 vec3 = {
                    .x = 0.0f,
                    .y = 0.0f,
                    .z = 0.0f,
                    .status = SensorStatus::ACCURACY_HIGH,
            };
            event.payload.set<EventPayload::Tag::vec3>(vec3);
            break;
        }
        case SensorType::MAGNETIC_FIELD: {
            EventPayload::Vec3 vec3 = {
                    .x = 100.0f,
                    .y = 0.0f,
                    .z = 50.0f,
                    .status = SensorStatus::ACCURACY_HIGH,
            };
            event.payload.set<EventPayload::Tag::vec3>(vec3);
            break;
        }
        case SensorType::LIGHT:
            event.payload.set<EventPayload::Tag::scalar>(80.0f);
            break;
        case SensorType::PROXIMITY:
            event.payload.set<EventPayload::Tag::scalar>(5.0f);
            break;
        case SensorType::PRESSURE:
            event.payload.set<EventPayload::Tag::scalar>(1013.25f);
            break;
        case SensorType::AMBIENT_TEMPERATURE:
            event.payload.set<EventPayload::Tag::scalar>(25.0f);
            break;
        case SensorType::RELATIVE_HUMIDITY:
            event.payload.set<EventPayload::Tag::scalar>(50.0f);
            break;
        default:
            return events;
    }

    events.push_back(event);
    return events;
}

int32_t MockBackend::Activate(int32_t sensor_handle, bool enabled) {
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
        sensor->poll_thread = std::thread(&MockBackend::PollSensorThread, this, sensor.get());
    } else {
        sensor->stop_thread = true;
        sensor->poll_cv.notify_all();
        if (sensor->poll_thread.joinable()) {
            sensor->poll_thread.join();
        }
    }

    LOG(INFO) << "Mock sensor " << sensor_handle << " " << (enabled ? "activated" : "deactivated");
    return 0;
}

int32_t MockBackend::Batch(int32_t sensor_handle, int64_t sampling_period_ns,
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

int32_t MockBackend::Flush(int32_t sensor_handle) {
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

int32_t MockBackend::SetOperationMode(OperationMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    operation_mode_ = mode;
    for (auto& [handle, sensor] : sensors_) {
        sensor->poll_cv.notify_all();
    }
    return 0;
}

}  // namespace aidl::android::hardware::sensors::mainline
