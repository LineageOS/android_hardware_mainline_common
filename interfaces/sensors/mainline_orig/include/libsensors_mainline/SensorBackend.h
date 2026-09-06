/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/ISensors.h>
#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <aidl/android/hardware/sensors/SensorStatus.h>
#include <aidl/android/hardware/sensors/SensorType.h>

#include <functional>
#include <string>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

using Event = ::aidl::android::hardware::sensors::Event;
using EventPayload = ::aidl::android::hardware::sensors::Event::EventPayload;
using OperationMode = ::aidl::android::hardware::sensors::ISensors::OperationMode;
using SensorInfo = ::aidl::android::hardware::sensors::SensorInfo;
using SensorStatus = ::aidl::android::hardware::sensors::SensorStatus;
using SensorType = ::aidl::android::hardware::sensors::SensorType;

using PostEventsCallback = std::function<void(const std::vector<Event>&, bool wakeup)>;

class ISensorBackend {
  public:
    virtual ~ISensorBackend() = default;

    virtual std::string GetName() const = 0;

    virtual int32_t Initialize(const PostEventsCallback& callback) = 0;

    virtual void Deinitialize() = 0;

    virtual std::vector<SensorInfo> GetSensorsList() = 0;

    virtual int32_t Activate(int32_t sensor_handle, bool enabled) = 0;

    virtual int32_t Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                          int64_t max_report_latency_ns) = 0;

    virtual int32_t Flush(int32_t sensor_handle) = 0;

    virtual int32_t SetOperationMode(OperationMode mode) = 0;
};

using CreateBackendFunc = ISensorBackend* (*)();

static constexpr const char* kCreateBackendSymbol = "CreateSensorBackend";

}  // namespace aidl::android::hardware::sensors::mainline
