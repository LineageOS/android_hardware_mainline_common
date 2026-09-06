/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/common/fmq/MQDescriptor.h>
#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <aidl/android/hardware/sensors/Event.h>
#include <fmq/AidlMessageQueue.h>
#include <fmq/EventFlag.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

/*
 * Delivers events to the framework through the Event FMQ and manages the
 * wake lock protocol of the Sensors HAL:
 *  - a wake lock is held while WAKE_UP events written to the FMQ have not been
 *    acknowledged by the framework through the Wake Lock FMQ,
 *  - it is released automatically WAKE_LOCK_TIMEOUT_SECONDS after the last
 *    WAKE_UP event if no acknowledgement arrives.
 */
class EventDispatcher {
  public:
    using Event = ::aidl::android::hardware::sensors::Event;
    using SynchronizedReadWrite = ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
    template <typename T>
    using MQDescriptor =
            ::aidl::android::hardware::common::fmq::MQDescriptor<T, SynchronizedReadWrite>;

    EventDispatcher();
    ~EventDispatcher();

    // (Re)creates the queues from the descriptors given by the framework.
    // Returns false if the descriptors are unusable.
    bool Initialize(const MQDescriptor<Event>& event_queue_descriptor,
                    const MQDescriptor<int32_t>& wake_lock_descriptor);

    // Drops the queues and releases any held wake lock.
    void Shutdown();

    // Writes the events to the FMQ and wakes the framework. Thread safe.
    void PostEvents(const std::vector<Event>& events, bool wakeup);

  private:
    void DeleteEventFlagLocked();
    void StopWakeLockThread();
    void WakeLockThread();
    void UpdateWakeLock(int32_t events_written, int32_t events_handled);

    std::mutex write_mutex_;
    std::unique_ptr<::android::AidlMessageQueue<Event, SynchronizedReadWrite>> event_queue_;
    std::unique_ptr<::android::AidlMessageQueue<int32_t, SynchronizedReadWrite>> wake_lock_queue_;
    ::android::hardware::EventFlag* event_queue_flag_ = nullptr;

    std::mutex wake_lock_mutex_;
    uint32_t outstanding_wake_up_events_ = 0;
    bool has_wake_lock_ = false;
    std::chrono::steady_clock::time_point auto_release_time_;
    std::thread wake_lock_thread_;
    std::atomic<bool> wake_lock_thread_run_{false};

    std::atomic<uint64_t> dropped_events_{0};
};

}  // namespace aidl::android::hardware::sensors::mainline
