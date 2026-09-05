/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensors"

#include "Sensors.h"

#include "DeviceOrientationSensor.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <utils/SystemClock.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

namespace aidl::android::hardware::sensors::mainline {

Sensors::Sensors()
    : event_queue_flag_(nullptr),
      outstanding_wake_up_events_(0),
      read_wake_lock_queue_run_(false),
      auto_release_wake_lock_time_(0),
      has_wake_lock_(false) {
    LOG(INFO) << "Mainline Sensors HAL initializing";

    if (::android::base::GetBoolProperty("vendor.sensors.composite.device_orientation.enabled",
                                         false)) {
        LOG(INFO) << "Enabling composite sensor: DeviceOrientationSensor";
        backend_manager_.RegisterCompositeSensor(std::make_unique<DeviceOrientationSensor>());
    }

    backend_manager_.LoadBackends();
}

Sensors::~Sensors() {
    LOG(INFO) << "Mainline Sensors HAL shutting down";
    backend_manager_.Deinitialize();
    read_wake_lock_queue_run_ = false;
    if (wake_lock_thread_.joinable()) {
        wake_lock_thread_.join();
    }
    DeleteEventFlag();
    {
        std::lock_guard<std::mutex> lock(write_lock_);
        event_queue_.reset();
        wake_lock_queue_.reset();
        callback_.reset();
    }
    ResetWakeLock();
}

::ndk::ScopedAStatus Sensors::activate(int32_t in_sensorHandle, bool in_enabled) {
    int32_t result = backend_manager_.Activate(in_sensorHandle, in_enabled);
    if (result == 0) {
        return ::ndk::ScopedAStatus::ok();
    }
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

::ndk::ScopedAStatus Sensors::batch(int32_t in_sensorHandle, int64_t in_samplingPeriodNs,
                                    int64_t in_maxReportLatencyNs) {
    int32_t result =
            backend_manager_.Batch(in_sensorHandle, in_samplingPeriodNs, in_maxReportLatencyNs);
    if (result == 0) {
        return ::ndk::ScopedAStatus::ok();
    }
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

::ndk::ScopedAStatus Sensors::configDirectReport(int32_t /* in_sensorHandle */,
                                                 int32_t /* in_channelHandle */,
                                                 ISensors::RateLevel /* in_rate */,
                                                 int32_t* _aidl_return) {
    *_aidl_return = EX_UNSUPPORTED_OPERATION;
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

::ndk::ScopedAStatus Sensors::flush(int32_t in_sensorHandle) {
    int32_t result = backend_manager_.Flush(in_sensorHandle);
    if (result == 0) {
        return ::ndk::ScopedAStatus::ok();
    }
    if (result == -EINVAL) {
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ::ndk::ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(BnSensors::ERROR_BAD_VALUE));
}

::ndk::ScopedAStatus Sensors::getSensorsList(
        std::vector<::aidl::android::hardware::sensors::SensorInfo>* _aidl_return) {
    *_aidl_return = backend_manager_.GetSensorsList();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Sensors::initialize(
        const MQDescriptor<Event, SynchronizedReadWrite>& in_eventQueueDescriptor,
        const MQDescriptor<int32_t, SynchronizedReadWrite>& in_wakeLockDescriptor,
        const std::shared_ptr<::aidl::android::hardware::sensors::ISensorsCallback>&
                in_sensorsCallback) {
    std::lock_guard<std::mutex> initialize_lock(initialize_mutex_);
    LOG(INFO) << "Sensors::initialize() called";

    backend_manager_.Deinitialize();
    read_wake_lock_queue_run_ = false;
    if (wake_lock_thread_.joinable()) {
        wake_lock_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(write_lock_);
        DeleteEventFlagLocked();
        event_queue_.reset();
        wake_lock_queue_.reset();
        callback_.reset();
    }
    ResetWakeLock();

    auto event_queue = std::make_unique<AidlMessageQueue<Event, SynchronizedReadWrite>>(
            in_eventQueueDescriptor, true);
    auto wake_lock_queue = std::make_unique<AidlMessageQueue<int32_t, SynchronizedReadWrite>>(
            in_wakeLockDescriptor, true);
    if (!in_sensorsCallback || !event_queue->isValid() || !wake_lock_queue->isValid() ||
        event_queue->getEventFlagWord() == nullptr ||
        wake_lock_queue->getEventFlagWord() == nullptr) {
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    EventFlag* event_queue_flag = nullptr;
    if (EventFlag::createEventFlag(event_queue->getEventFlagWord(), &event_queue_flag) != OK ||
        event_queue_flag == nullptr) {
        if (event_queue_flag != nullptr) {
            EventFlag::deleteEventFlag(&event_queue_flag);
        }
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    {
        std::lock_guard<std::mutex> lock(write_lock_);
        event_queue_ = std::move(event_queue);
        wake_lock_queue_ = std::move(wake_lock_queue);
        event_queue_flag_ = event_queue_flag;
        callback_ = in_sensorsCallback;
    }

    auto post_callback = [this](const std::vector<Event>& events, bool wakeup) {
        PostEvents(events, wakeup);
    };

    backend_manager_.Initialize(post_callback);

    read_wake_lock_queue_run_ = true;
    wake_lock_thread_ = std::thread(StartReadWakeLockThread, this);

    LOG(INFO) << "Sensors::initialize() completed";
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus Sensors::injectSensorData(const Event& /* in_event */) {
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

::ndk::ScopedAStatus Sensors::registerDirectChannel(const ISensors::SharedMemInfo& /* in_mem */,
                                                    int32_t* _aidl_return) {
    *_aidl_return = EX_UNSUPPORTED_OPERATION;
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

::ndk::ScopedAStatus Sensors::setOperationMode(ISensors::OperationMode in_mode) {
    if (in_mode == ISensors::OperationMode::DATA_INJECTION) {
        return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    int32_t result = backend_manager_.SetOperationMode(in_mode);
    if (result == 0) {
        return ::ndk::ScopedAStatus::ok();
    }
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

::ndk::ScopedAStatus Sensors::unregisterDirectChannel(int32_t /* in_channelHandle */) {
    return ::ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

void Sensors::PostEvents(const std::vector<Event>& events, bool wakeup) {
    if (events.empty()) return;

    std::lock_guard<std::mutex> lock(write_lock_);
    if (event_queue_ == nullptr || event_queue_flag_ == nullptr) {
        return;
    }

    const size_t queue_capacity = event_queue_->getQuantumCount();
    if (queue_capacity == 0) return;

    constexpr int64_t kWriteTimeoutNs = 500 * 1000 * 1000;
    size_t offset = 0;
    while (offset < events.size()) {
        const size_t count = std::min(queue_capacity, events.size() - offset);
        if (wakeup) UpdateWakeLock(count, 0);
        if (!event_queue_->writeBlocking(
                    events.data() + offset, count,
                    static_cast<uint32_t>(BnSensors::EVENT_QUEUE_FLAG_BITS_EVENTS_READ),
                    static_cast<uint32_t>(BnSensors::EVENT_QUEUE_FLAG_BITS_READ_AND_PROCESS),
                    kWriteTimeoutNs, event_queue_flag_)) {
            LOG(ERROR) << "Failed to write " << (events.size() - offset)
                       << " sensor event(s) after waiting for FMQ space";
            if (wakeup) UpdateWakeLock(0, count);
            return;
        }
        if (wakeup) RefreshWakeLockTimeout();
        offset += count;
    }
}

void Sensors::DeleteEventFlag() {
    std::lock_guard<std::mutex> lock(write_lock_);
    DeleteEventFlagLocked();
}

void Sensors::DeleteEventFlagLocked() {
    if (event_queue_flag_ != nullptr) {
        status_t status = EventFlag::deleteEventFlag(&event_queue_flag_);
        if (status != OK) {
            LOG(WARNING) << "Failed to delete event flag: " << status;
        }
    }
}

void Sensors::StartReadWakeLockThread(Sensors* sensors) {
    sensors->ReadWakeLockFMQ();
}

void Sensors::ReadWakeLockFMQ() {
    while (read_wake_lock_queue_run_.load()) {
        constexpr int64_t kReadTimeoutNs = 500 * 1000 * 1000;
        int32_t events_handled = 0;

        const bool read_succeeded = wake_lock_queue_->readBlocking(
                &events_handled, 1, 0,
                static_cast<uint32_t>(BnSensors::WAKE_LOCK_QUEUE_FLAG_BITS_DATA_WRITTEN),
                kReadTimeoutNs);
        if (read_succeeded && events_handled > 0) {
            UpdateWakeLock(0, static_cast<uint64_t>(events_handled));
        } else {
            UpdateWakeLock(0, 0);
        }
    }
}

void Sensors::UpdateWakeLock(uint64_t events_written, uint64_t events_handled) {
    std::lock_guard<std::mutex> lock(wake_lock_mutex_);
    if (events_handled >= outstanding_wake_up_events_) {
        outstanding_wake_up_events_ = 0;
    } else {
        outstanding_wake_up_events_ -= events_handled;
    }
    if (events_written > std::numeric_limits<uint64_t>::max() - outstanding_wake_up_events_) {
        outstanding_wake_up_events_ = std::numeric_limits<uint64_t>::max();
    } else {
        outstanding_wake_up_events_ += events_written;
    }

    if (events_written > 0) {
        auto_release_wake_lock_time_ = ::android::uptimeMillis() + WAKE_LOCK_TIMEOUT_SECONDS * 1000;
    }

    if (!has_wake_lock_ && outstanding_wake_up_events_ > 0 && AcquireWakeLock()) {
        has_wake_lock_ = true;
    } else if (has_wake_lock_) {
        if (::android::uptimeMillis() >= auto_release_wake_lock_time_) {
            LOG(DEBUG) << "No events read from wake lock FMQ for " << WAKE_LOCK_TIMEOUT_SECONDS
                       << " seconds, auto releasing wake lock";
            outstanding_wake_up_events_ = 0;
        }

        if (outstanding_wake_up_events_ == 0 && ReleaseWakeLock()) {
            has_wake_lock_ = false;
        }
    }
}

void Sensors::RefreshWakeLockTimeout() {
    std::lock_guard<std::mutex> lock(wake_lock_mutex_);
    if (outstanding_wake_up_events_ > 0) {
        auto_release_wake_lock_time_ = ::android::uptimeMillis() + WAKE_LOCK_TIMEOUT_SECONDS * 1000;
    }
}

void Sensors::ResetWakeLock() {
    std::lock_guard<std::mutex> lock(wake_lock_mutex_);
    if (has_wake_lock_ && !ReleaseWakeLock()) {
        LOG(WARNING) << "Failed to release wake lock while resetting state";
    }
    outstanding_wake_up_events_ = 0;
    auto_release_wake_lock_time_ = 0;
    has_wake_lock_ = false;
}

bool Sensors::AcquireWakeLock() {
    return ::android::base::WriteStringToFile(kWakeLockName, kWakeLockPath);
}

bool Sensors::ReleaseWakeLock() {
    return ::android::base::WriteStringToFile(kWakeLockName, kWakeUnlockPath);
}

}  // namespace aidl::android::hardware::sensors::mainline
