/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/sensors/BnSensors.h>

#include <memory>
#include <mutex>

#include "EventDispatcher.h"
#include "SensorManager.h"

namespace aidl::android::hardware::sensors::mainline {

/*
 * android.hardware.sensors ISensors implementation.
 *
 * Translates the AIDL calls into SensorManager operations and their errno
 * style results into binder statuses. Events flow from the backends through
 * SensorManager into EventDispatcher.
 */
class Sensors : public BnSensors {
  public:
    Sensors();
    ~Sensors() override;

    // Loads backends and discovers sensors; call before registering the
    // service so that getSensorsList() is complete from the start.
    void Initialize();

    ::ndk::ScopedAStatus activate(int32_t in_sensorHandle, bool in_enabled) override;
    ::ndk::ScopedAStatus batch(int32_t in_sensorHandle, int64_t in_samplingPeriodNs,
                               int64_t in_maxReportLatencyNs) override;
    ::ndk::ScopedAStatus configDirectReport(int32_t in_sensorHandle, int32_t in_channelHandle,
                                            ISensors::RateLevel in_rate,
                                            int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus flush(int32_t in_sensorHandle) override;
    ::ndk::ScopedAStatus getSensorsList(std::vector<SensorInfo>* _aidl_return) override;
    ::ndk::ScopedAStatus initialize(
            const ::aidl::android::hardware::common::fmq::MQDescriptor<
                    Event, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                    in_eventQueueDescriptor,
            const ::aidl::android::hardware::common::fmq::MQDescriptor<
                    int32_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                    in_wakeLockDescriptor,
            const std::shared_ptr<ISensorsCallback>& in_sensorsCallback) override;
    ::ndk::ScopedAStatus injectSensorData(const Event& in_event) override;
    ::ndk::ScopedAStatus registerDirectChannel(const ISensors::SharedMemInfo& in_mem,
                                               int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus setOperationMode(ISensors::OperationMode in_mode) override;
    ::ndk::ScopedAStatus unregisterDirectChannel(int32_t in_channelHandle) override;

  private:
    static ::ndk::ScopedAStatus StatusFromErrno(int32_t result);

    SensorManager manager_;
    EventDispatcher dispatcher_;
    std::mutex initialize_mutex_;
    std::shared_ptr<ISensorsCallback> callback_;
};

}  // namespace aidl::android::hardware::sensors::mainline
