/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_mainline/SensorBackend.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "BackendLoader.h"
#include "composite/CompositeSensor.h"

namespace aidl::android::hardware::sensors::mainline {

/*
 * Owns the backends and the composite sensors and presents them as one flat
 * list of sensors with global handles.
 *
 * Responsibilities:
 *  - loading and initialising the backends, assigning global handles,
 *  - routing activate/batch/flush requests to the right backend with
 *    backend-local handles,
 *  - tracking who needs a hardware sensor (the framework and/or composite
 *    sensors) and driving the backend accordingly (union of activations,
 *    fastest requested rate),
 *  - remapping incoming events, feeding composite sensors and forwarding to
 *    the event sink,
 *  - data injection mode.
 *
 * Locking: control_mutex_ serialises control operations and is held while
 * calling into backends; state_mutex_ protects the tables and is never held
 * while calling into a backend, so that backend threads delivering events
 * (which take state_mutex_) can never deadlock with a control operation.
 */
class SensorManager {
  public:
    SensorManager();
    ~SensorManager();

    SensorManager(const SensorManager&) = delete;
    SensorManager& operator=(const SensorManager&) = delete;

    // Composite sensors must be registered before Initialize().
    void RegisterCompositeSensor(std::unique_ptr<ICompositeSensor> sensor);

    // Loads the backends and discovers the sensors. Call once.
    void Initialize();

    // Destination of events. May be replaced at any time (nullptr to drop).
    void SetEventSink(PostEventsCallback sink);

    // Deactivates every sensor (framework re-initialisation).
    void DeactivateAll();

    std::vector<SensorInfo> GetSensorsList() const;
    std::optional<SensorInfo> GetSensorInfo(int32_t handle) const;

    // All methods return 0 on success or a negative errno (-EINVAL for an
    // unknown handle or an invalid request, -ENOTSUP for unsupported
    // operations).
    int32_t Activate(int32_t handle, bool enabled);
    int32_t Batch(int32_t handle, int64_t sampling_period_ns, int64_t max_report_latency_ns);
    int32_t Flush(int32_t handle);
    int32_t SetOperationMode(OperationMode mode);
    int32_t InjectEvent(const Event& event);
    bool SupportsDataInjection() const;

  private:
    struct BackendEntry {
        LoadedBackend loaded;
        bool initialized = false;
        std::map<int32_t, int32_t> local_to_global;
    };

    struct HardwareSensor {
        int32_t handle = 0;
        size_t backend_index = 0;
        int32_t local_handle = 0;
        SensorInfo info;
        // Requests.
        bool client_active = false;
        int64_t client_period_ns = 0;
        int64_t client_latency_ns = 0;
        std::map<int32_t, int64_t> composite_periods;  // composite handle -> period
        // What the backend currently does.
        bool backend_active = false;
        int64_t backend_period_ns = -1;
        int64_t backend_latency_ns = -1;
        // Composite sensors consuming this sensor's events.
        std::vector<int32_t> composite_subscribers;
    };

    struct CompositeEntry {
        int32_t handle = 0;
        std::unique_ptr<ICompositeSensor> sensor;
        std::vector<int32_t> input_handles;
    };

    void InitializeBackends();
    void ShutdownBackends();
    void RegisterHardwareSensors();
    void RegisterComposites();
    void OnBackendEvents(size_t backend_index, const std::vector<Event>& events, bool wakeup);
    void Deliver(const std::vector<Event>& events, bool wakeup);

    int32_t ApplyHardwareState(int32_t handle);
    int32_t ActivateComposite(CompositeEntry* composite, bool enabled);

    HardwareSensor* FindHardware(int32_t handle);
    const HardwareSensor* FindHardware(int32_t handle) const;
    CompositeEntry* FindComposite(int32_t handle);

    mutable std::mutex control_mutex_;
    mutable std::mutex state_mutex_;

    std::vector<BackendEntry> backends_;
    std::map<int32_t, HardwareSensor> hardware_;
    std::vector<CompositeEntry> composites_;
    std::vector<std::unique_ptr<ICompositeSensor>> pending_composites_;
    int32_t next_handle_ = 1;
    PostEventsCallback sink_;
    OperationMode mode_ = OperationMode::NORMAL;
    bool initialized_ = false;
    bool log_events_ = false;
};

}  // namespace aidl::android::hardware::sensors::mainline
