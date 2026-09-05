/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/common/fmq/SynchronizedReadWrite.h>
#include <aidl/android/hardware/sensors/BnSensors.h>
#include <fmq/AidlMessageQueue.h>

#include <libsensors_mainline/SensorBackend.h>

#include "SensorBackendManager.h"

#include <android-base/thread_annotations.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace aidl::android::hardware::sensors::mainline {

using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ::android::AidlMessageQueue;
using ::android::OK;
using ::android::status_t;
using ::android::hardware::EventFlag;

class Sensors : public BnSensors {
    static constexpr const char* kWakeLockName = "SensorsHAL_WAKEUP_Mainline";
    static constexpr const char* kWakeLockPath = "/sys/power/wake_lock";
    static constexpr const char* kWakeUnlockPath = "/sys/power/wake_unlock";
    static constexpr int32_t WAKE_LOCK_TIMEOUT_SECONDS = 1;

  public:
    Sensors();
    ~Sensors() override;

    ::ndk::ScopedAStatus activate(int32_t in_sensorHandle, bool in_enabled) override;
    ::ndk::ScopedAStatus batch(int32_t in_sensorHandle, int64_t in_samplingPeriodNs,
                               int64_t in_maxReportLatencyNs) override;
    ::ndk::ScopedAStatus configDirectReport(
            int32_t in_sensorHandle, int32_t in_channelHandle,
            ::aidl::android::hardware::sensors::ISensors::RateLevel in_rate,
            int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus flush(int32_t in_sensorHandle) override;
    ::ndk::ScopedAStatus getSensorsList(
            std::vector<::aidl::android::hardware::sensors::SensorInfo>* _aidl_return) override;
    ::ndk::ScopedAStatus initialize(
            const ::aidl::android::hardware::common::fmq::MQDescriptor<
                    ::aidl::android::hardware::sensors::Event,
                    ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                    in_eventQueueDescriptor,
            const ::aidl::android::hardware::common::fmq::MQDescriptor<
                    int32_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                    in_wakeLockDescriptor,
            const std::shared_ptr<::aidl::android::hardware::sensors::ISensorsCallback>&
                    in_sensorsCallback) override;
    ::ndk::ScopedAStatus injectSensorData(
            const ::aidl::android::hardware::sensors::Event& in_event) override;
    ::ndk::ScopedAStatus registerDirectChannel(
            const ::aidl::android::hardware::sensors::ISensors::SharedMemInfo& in_mem,
            int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus setOperationMode(
            ::aidl::android::hardware::sensors::ISensors::OperationMode in_mode) override;
    ::ndk::ScopedAStatus unregisterDirectChannel(int32_t in_channelHandle) override;

  private:
    void PostEvents(const std::vector<Event>& events, bool wakeup);

    void DeleteEventFlag();
    void DeleteEventFlagLocked() REQUIRES(write_lock_);

    static void StartReadWakeLockThread(Sensors* sensors);
    void ReadWakeLockFMQ();
    void UpdateWakeLock(uint64_t events_written, uint64_t events_handled);
    void RefreshWakeLockTimeout();
    void ResetWakeLock();

    bool AcquireWakeLock();
    bool ReleaseWakeLock();

    SensorBackendManager backend_manager_;

    std::unique_ptr<AidlMessageQueue<Event, SynchronizedReadWrite>> event_queue_;
    std::unique_ptr<AidlMessageQueue<int32_t, SynchronizedReadWrite>> wake_lock_queue_;
    EventFlag* event_queue_flag_ GUARDED_BY(write_lock_);

    std::shared_ptr<::aidl::android::hardware::sensors::ISensorsCallback> callback_;

    std::mutex write_lock_;
    std::mutex wake_lock_mutex_;
    std::mutex initialize_mutex_;

    uint64_t outstanding_wake_up_events_;
    std::thread wake_lock_thread_;
    std::atomic_bool read_wake_lock_queue_run_;
    int64_t auto_release_wake_lock_time_;
    bool has_wake_lock_;
};

}  // namespace aidl::android::hardware::sensors::mainline
