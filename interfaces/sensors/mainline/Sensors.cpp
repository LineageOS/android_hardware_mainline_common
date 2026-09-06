/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensors"

#include "Sensors.h"

#include <android-base/logging.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/Settings.h>

#include <cerrno>

#include "composite/DeviceOrientationSensor.h"

namespace aidl::android::hardware::sensors::mainline {

using ::ndk::ScopedAStatus;

Sensors::Sensors() = default;

Sensors::~Sensors() {
    LOG(INFO) << "Sensors HAL shutting down";
    manager_.SetEventSink(nullptr);
    dispatcher_.Shutdown();
}

void Sensors::Initialize() {
    LOG(INFO) << "Sensors HAL starting";
    for (const auto& file : Settings::Get().GetLoadedFiles()) {
        LOG(INFO) << "Configuration file: " << file;
    }

    if (Settings::Get().GetBool("composite.device_orientation.enabled", false)) {
        LOG(INFO) << "Composite device orientation sensor enabled";
        manager_.RegisterCompositeSensor(std::make_unique<DeviceOrientationSensor>());
    }

    manager_.Initialize();
    manager_.SetEventSink([this](const std::vector<Event>& events, bool wakeup) {
        dispatcher_.PostEvents(events, wakeup);
    });

    for (const auto& info : manager_.GetSensorsList()) {
        LOG(INFO) << "Exposed sensor: " << SensorInfoToString(info);
    }
}

ScopedAStatus Sensors::StatusFromErrno(int32_t result) {
    switch (result) {
        case 0:
            return ScopedAStatus::ok();
        case -EINVAL:
            return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        case -ENOTSUP:
            return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        default:
            return ScopedAStatus::fromServiceSpecificError(ISensors::ERROR_BAD_VALUE);
    }
}

ScopedAStatus Sensors::activate(int32_t in_sensorHandle, bool in_enabled) {
    LOG(DEBUG) << "activate(" << in_sensorHandle << ", " << in_enabled << ")";
    int32_t ret = manager_.Activate(in_sensorHandle, in_enabled);
    if (ret != 0 && ret != -EINVAL) {
        // Hardware failure: report as illegal argument like the reference
        // implementation, the framework has no better recovery path.
        LOG(ERROR) << "activate(" << in_sensorHandle << ", " << in_enabled << ") failed: " << ret;
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return StatusFromErrno(ret);
}

ScopedAStatus Sensors::batch(int32_t in_sensorHandle, int64_t in_samplingPeriodNs,
                             int64_t in_maxReportLatencyNs) {
    LOG(DEBUG) << "batch(" << in_sensorHandle << ", " << in_samplingPeriodNs << ", "
               << in_maxReportLatencyNs << ")";
    int32_t ret = manager_.Batch(in_sensorHandle, in_samplingPeriodNs, in_maxReportLatencyNs);
    if (ret != 0 && ret != -EINVAL) {
        LOG(WARNING) << "batch(" << in_sensorHandle << ") backend failure " << ret
                     << ", rate kept unchanged";
        return ScopedAStatus::ok();
    }
    return StatusFromErrno(ret);
}

ScopedAStatus Sensors::configDirectReport(int32_t /* in_sensorHandle */,
                                          int32_t /* in_channelHandle */,
                                          ISensors::RateLevel /* in_rate */,
                                          int32_t* _aidl_return) {
    *_aidl_return = 0;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus Sensors::flush(int32_t in_sensorHandle) {
    LOG(DEBUG) << "flush(" << in_sensorHandle << ")";
    return StatusFromErrno(manager_.Flush(in_sensorHandle));
}

ScopedAStatus Sensors::getSensorsList(std::vector<SensorInfo>* _aidl_return) {
    *_aidl_return = manager_.GetSensorsList();
    return ScopedAStatus::ok();
}

ScopedAStatus Sensors::initialize(
        const ::aidl::android::hardware::common::fmq::MQDescriptor<
                Event, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                in_eventQueueDescriptor,
        const ::aidl::android::hardware::common::fmq::MQDescriptor<
                int32_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>&
                in_wakeLockDescriptor,
        const std::shared_ptr<ISensorsCallback>& in_sensorsCallback) {
    std::lock_guard<std::mutex> lock(initialize_mutex_);
    LOG(INFO) << "initialize()";

    // All active sensor requests must be closed on (re)initialisation.
    manager_.DeactivateAll();
    callback_ = in_sensorsCallback;

    if (!in_sensorsCallback) {
        LOG(ERROR) << "initialize(): null callback";
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (!dispatcher_.Initialize(in_eventQueueDescriptor, in_wakeLockDescriptor)) {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Sensors::injectSensorData(const Event& in_event) {
    int32_t ret = manager_.InjectEvent(in_event);
    switch (ret) {
        case 0:
            return ScopedAStatus::ok();
        case -ENOTSUP:
            return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        default:
            return ScopedAStatus::fromServiceSpecificError(ISensors::ERROR_BAD_VALUE);
    }
}

ScopedAStatus Sensors::registerDirectChannel(const ISensors::SharedMemInfo& /* in_mem */,
                                             int32_t* _aidl_return) {
    *_aidl_return = 0;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus Sensors::setOperationMode(ISensors::OperationMode in_mode) {
    LOG(INFO) << "setOperationMode(" << toString(in_mode) << ")";
    return StatusFromErrno(manager_.SetOperationMode(in_mode));
}

ScopedAStatus Sensors::unregisterDirectChannel(int32_t /* in_channelHandle */) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace aidl::android::hardware::sensors::mainline
