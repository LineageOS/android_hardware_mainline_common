/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Backend interface of the mainline Sensors HAL.
 *
 * The frontend (android.hardware.sensors-service.mainline) dlopen()s backend
 * shared libraries named libsensors_<name>.so and looks up the symbols declared
 * at the bottom of this file. A backend bridges the frontend with one Linux
 * subsystem (IIO, input, ...) or with an external sensor service.
 *
 * Contract summary:
 *  - Handles returned in SensorInfo::sensorHandle are local to the backend and
 *    must be stable for the lifetime of the process. The frontend maps them to
 *    global handles; backends never see global handles.
 *  - Event::sensorHandle must contain the backend-local handle.
 *  - Event::timestamp must be in CLOCK_BOOTTIME nanoseconds
 *    (Android "elapsedRealtimeNano" time base).
 *  - Events are pushed through the PostEventsCallback given to Initialize(); it
 *    may be invoked from any backend thread and is thread safe.
 *  - The frontend performs generic argument validation (unknown handle, flush of
 *    an inactive or one-shot sensor, ...) before calling into the backend, and
 *    also handles data injection and composite sensors itself. Backends only
 *    deal with the hardware.
 *
 * ABI stability: the virtual method layout of ISensorBackend is part of the
 * ABI between the frontend and out-of-tree backends. Methods must not be
 * reordered, removed or inserted in the middle. New optional functionality is
 * exposed through additional exported C symbols (see GetSensorBackendFlags()).
 */

#include <aidl/android/hardware/sensors/Event.h>
#include <aidl/android/hardware/sensors/ISensors.h>
#include <aidl/android/hardware/sensors/SensorInfo.h>
#include <aidl/android/hardware/sensors/SensorStatus.h>
#include <aidl/android/hardware/sensors/SensorType.h>

#include <cstdint>
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

// Callback used by backends to deliver events to the frontend.
// `wakeup` shall be true if the batch contains events of a WAKE_UP sensor.
using PostEventsCallback = std::function<void(const std::vector<Event>& events, bool wakeup)>;

// Version of this interface. Bump when the ISensorBackend layout changes.
inline constexpr uint32_t kSensorBackendInterfaceVersion = 1;

// Flags returned by the optional GetSensorBackendFlags() symbol.
enum SensorBackendFlags : uint32_t {
    // The backend only provides fallback sensors (e.g. fake data). The frontend
    // skips its sensors whose type is already provided by a previously loaded
    // backend.
    kSensorBackendFlagFallbackOnly = 1u << 0,
};

// Return value of ISensorBackend::Flush() telling the frontend that the backend
// has no FIFO and that the frontend shall emit the FLUSH_COMPLETE event itself.
inline constexpr int32_t kFlushHandledByFrontend = -1000;

class ISensorBackend {
  public:
    virtual ~ISensorBackend() = default;

    // Short human readable backend name used in logs, e.g. "iio".
    virtual std::string GetName() const = 0;

    // Discovers the hardware and prepares the backend for use. Called exactly
    // once, before any other method. Returns 0 on success or a negative errno.
    // A backend which finds no sensor shall still return 0 and provide an
    // empty sensor list.
    virtual int32_t Initialize(const PostEventsCallback& callback) = 0;

    // Deactivates every sensor and releases all resources. Called once before
    // the backend is destroyed. No events may be posted after this returns.
    virtual void Deinitialize() = 0;

    // Returns the list of sensors. The list and its contents must not change
    // after Initialize() returned.
    virtual std::vector<SensorInfo> GetSensorsList() = 0;

    // Activates or deactivates a sensor. The frontend never activates an
    // already active sensor nor deactivates an inactive one. Deactivation must
    // drop any pending events of the sensor. Returns 0 or a negative errno.
    virtual int32_t Activate(int32_t sensor_handle, bool enabled) = 0;

    // Sets the sampling period and the maximum report latency of a sensor. May
    // be called before activation and while active. Values are already clamped
    // to [minDelayUs, maxDelayUs] by the frontend. Returns 0 or a negative
    // errno.
    virtual int32_t Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                          int64_t max_report_latency_ns) = 0;

    // Flushes the FIFO of an active, non one-shot sensor. The backend must post
    // any batched events followed by a META_DATA FLUSH_COMPLETE event for the
    // sensor, and return 0. A backend without FIFO may return
    // kFlushHandledByFrontend to let the frontend post the FLUSH_COMPLETE event.
    // Any other negative value is reported as an error to the framework.
    virtual int32_t Flush(int32_t sensor_handle) = 0;

    // Informs the backend of the operation mode. In DATA_INJECTION mode the
    // frontend discards every event coming from backends; backends may use this
    // notification to save power but are not required to. Returns 0 or a
    // negative errno.
    virtual int32_t SetOperationMode(OperationMode mode) = 0;
};

// Exported symbols of a backend library.
using CreateBackendFunc = ISensorBackend* (*)();
using GetBackendInterfaceVersionFunc = uint32_t (*)();
using GetBackendFlagsFunc = uint32_t (*)();

// Mandatory: creates a backend instance. The frontend takes ownership.
inline constexpr const char* kCreateBackendSymbol = "CreateSensorBackend";
// Optional: returns kSensorBackendInterfaceVersion the library was built with.
// Missing symbol means version 1.
inline constexpr const char* kBackendInterfaceVersionSymbol = "GetSensorBackendInterfaceVersion";
// Optional: returns a bitwise OR of SensorBackendFlags. Missing symbol means 0.
inline constexpr const char* kBackendFlagsSymbol = "GetSensorBackendFlags";

}  // namespace aidl::android::hardware::sensors::mainline

// Helper for backend libraries: defines the exported symbols for a backend
// class `Type` with the given interface flags.
#define DEFINE_SENSOR_BACKEND(Type, flags)                                                        \
    extern "C" __attribute__((                                                                    \
            visibility("default"))) ::aidl::android::hardware::sensors::mainline::ISensorBackend* \
    CreateSensorBackend() {                                                                       \
        return new Type();                                                                        \
    }                                                                                             \
    extern "C" __attribute__((visibility("default"))) uint32_t                                    \
    GetSensorBackendInterfaceVersion() {                                                          \
        return ::aidl::android::hardware::sensors::mainline::kSensorBackendInterfaceVersion;      \
    }                                                                                             \
    extern "C" __attribute__((visibility("default"))) uint32_t GetSensorBackendFlags() {          \
        return (flags);                                                                           \
    }
