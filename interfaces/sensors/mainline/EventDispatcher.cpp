/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsDispatcher"

#include "EventDispatcher.h"

#include <aidl/android/hardware/sensors/ISensors.h>
#include <android-base/logging.h>
#include <hardware_legacy/power.h>

#include <pthread.h>

namespace aidl::android::hardware::sensors::mainline {

namespace {

using ::aidl::android::hardware::sensors::ISensors;

// The name of the wake lock must begin with "SensorsHAL_WAKEUP".
constexpr const char* kWakeLockName = "SensorsHAL_WAKEUP_mainline";
constexpr int64_t kWakeLockReadTimeoutNs = 500LL * 1000 * 1000;
constexpr uint64_t kDropLogInterval = 100;

}  // namespace

EventDispatcher::EventDispatcher() = default;

EventDispatcher::~EventDispatcher() {
    Shutdown();
}

bool EventDispatcher::Initialize(const MQDescriptor<Event>& event_queue_descriptor,
                                 const MQDescriptor<int32_t>& wake_lock_descriptor) {
    StopWakeLockThread();

    bool ok = true;
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        DeleteEventFlagLocked();

        event_queue_ = std::make_unique<::android::AidlMessageQueue<Event, SynchronizedReadWrite>>(
                event_queue_descriptor, true /* resetPointers */);
        wake_lock_queue_ =
                std::make_unique<::android::AidlMessageQueue<int32_t, SynchronizedReadWrite>>(
                        wake_lock_descriptor, true /* resetPointers */);

        if (!event_queue_->isValid() || !wake_lock_queue_->isValid()) {
            LOG(ERROR) << "Invalid FMQ descriptor(s): event queue valid=" << event_queue_->isValid()
                       << " wake lock queue valid=" << wake_lock_queue_->isValid();
            ok = false;
        } else if (::android::hardware::EventFlag::createEventFlag(
                           event_queue_->getEventFlagWord(), &event_queue_flag_) != ::android::OK) {
            LOG(ERROR) << "Failed to create the event queue flag";
            event_queue_flag_ = nullptr;
            ok = false;
        }
        if (!ok) {
            event_queue_.reset();
            wake_lock_queue_.reset();
        }
    }

    {
        std::lock_guard<std::mutex> lock(wake_lock_mutex_);
        outstanding_wake_up_events_ = 0;
        if (has_wake_lock_) {
            release_wake_lock(kWakeLockName);
            has_wake_lock_ = false;
        }
    }

    if (ok) {
        wake_lock_thread_run_.store(true);
        wake_lock_thread_ = std::thread(&EventDispatcher::WakeLockThread, this);
        LOG(INFO) << "Event queue initialized (capacity " << event_queue_->getQuantumCount()
                  << " events)";
    }
    return ok;
}

void EventDispatcher::Shutdown() {
    StopWakeLockThread();
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        DeleteEventFlagLocked();
        event_queue_.reset();
        wake_lock_queue_.reset();
    }
    std::lock_guard<std::mutex> lock(wake_lock_mutex_);
    outstanding_wake_up_events_ = 0;
    if (has_wake_lock_) {
        release_wake_lock(kWakeLockName);
        has_wake_lock_ = false;
    }
}

void EventDispatcher::DeleteEventFlagLocked() {
    if (event_queue_flag_ != nullptr) {
        ::android::status_t status =
                ::android::hardware::EventFlag::deleteEventFlag(&event_queue_flag_);
        if (status != ::android::OK) {
            LOG(WARNING) << "Failed to delete the event queue flag: " << status;
        }
        event_queue_flag_ = nullptr;
    }
}

void EventDispatcher::StopWakeLockThread() {
    wake_lock_thread_run_.store(false);
    if (wake_lock_thread_.joinable()) {
        wake_lock_thread_.join();
    }
}

void EventDispatcher::PostEvents(const std::vector<Event>& events, bool wakeup) {
    if (events.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!event_queue_ || event_queue_flag_ == nullptr) {
        return;
    }
    if (!event_queue_->write(events.data(), events.size())) {
        uint64_t dropped = dropped_events_.fetch_add(events.size()) + events.size();
        if (dropped % kDropLogInterval < events.size()) {
            LOG(WARNING) << "Event queue full, " << dropped
                         << " event(s) dropped so far (framework not reading?)";
        }
        return;
    }
    event_queue_flag_->wake(
            static_cast<uint32_t>(ISensors::EVENT_QUEUE_FLAG_BITS_READ_AND_PROCESS));
    if (wakeup) {
        UpdateWakeLock(static_cast<int32_t>(events.size()), 0);
    }
}

void EventDispatcher::WakeLockThread() {
    pthread_setname_np(pthread_self(), "sensors-wakelk");
    while (wake_lock_thread_run_.load()) {
        int32_t events_handled = 0;
        // The queue pointer is stable while this thread runs: Initialize() and
        // Shutdown() stop the thread before touching it.
        wake_lock_queue_->readBlocking(
                &events_handled, 1, 0 /* readNotification */,
                static_cast<uint32_t>(ISensors::WAKE_LOCK_QUEUE_FLAG_BITS_DATA_WRITTEN),
                kWakeLockReadTimeoutNs);
        UpdateWakeLock(0, events_handled);
    }
}

void EventDispatcher::UpdateWakeLock(int32_t events_written, int32_t events_handled) {
    std::lock_guard<std::mutex> lock(wake_lock_mutex_);
    const int64_t updated =
            static_cast<int64_t>(outstanding_wake_up_events_) + events_written - events_handled;
    outstanding_wake_up_events_ = updated < 0 ? 0 : static_cast<uint32_t>(updated);

    const auto now = std::chrono::steady_clock::now();
    if (events_written > 0) {
        auto_release_time_ = now + std::chrono::seconds(ISensors::WAKE_LOCK_TIMEOUT_SECONDS);
    }

    if (!has_wake_lock_ && outstanding_wake_up_events_ > 0) {
        if (acquire_wake_lock(PARTIAL_WAKE_LOCK, kWakeLockName) == 0) {
            has_wake_lock_ = true;
            LOG(DEBUG) << "Wake lock acquired";
        } else {
            LOG(WARNING) << "Failed to acquire wake lock " << kWakeLockName;
        }
    } else if (has_wake_lock_) {
        if (now > auto_release_time_) {
            LOG(DEBUG) << "No wake lock acknowledgement for " << ISensors::WAKE_LOCK_TIMEOUT_SECONDS
                       << " s, releasing the wake lock";
            outstanding_wake_up_events_ = 0;
        }
        if (outstanding_wake_up_events_ == 0 && release_wake_lock(kWakeLockName) == 0) {
            has_wake_lock_ = false;
            LOG(DEBUG) << "Wake lock released";
        }
    }
}

}  // namespace aidl::android::hardware::sensors::mainline
