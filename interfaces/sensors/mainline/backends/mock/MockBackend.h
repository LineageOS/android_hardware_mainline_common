/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_common/PeriodicWorker.h>
#include <libsensors_mainline/SensorBackend.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace aidl::android::hardware::sensors::mainline {

/*
 * Backend providing fake data for a configurable set of sensor types.
 *
 * It is flagged "fallback only": the frontend drops mock sensors whose type is
 * already provided by a real backend. The set of created sensors can be
 * restricted with the "mock.sensors" setting (comma separated type names, e.g.
 * "accel,gyro,light"); "none" disables the backend entirely.
 */
class MockBackend : public ISensorBackend {
  public:
    MockBackend();
    ~MockBackend() override;

    std::string GetName() const override;
    int32_t Initialize(const PostEventsCallback& callback) override;
    void Deinitialize() override;
    std::vector<SensorInfo> GetSensorsList() override;
    int32_t Activate(int32_t sensor_handle, bool enabled) override;
    int32_t Batch(int32_t sensor_handle, int64_t sampling_period_ns,
                  int64_t max_report_latency_ns) override;
    int32_t Flush(int32_t sensor_handle) override;
    int32_t SetOperationMode(OperationMode mode) override;

  private:
    struct MockSensor {
        SensorInfo info;
        int64_t period_ns;
        bool active = false;
        uint64_t sample_count = 0;
        std::optional<Event> last_event;
        std::unique_ptr<PeriodicWorker> worker;
    };

    void CreateSensor(SensorType type);
    void GenerateSample(MockSensor* sensor);
    Event BuildEvent(MockSensor* sensor, int64_t timestamp_ns);

    std::mutex mutex_;
    std::map<int32_t, std::unique_ptr<MockSensor>> sensors_;
    int32_t next_handle_ = 1;
    PostEventsCallback post_events_;
    std::atomic<OperationMode> operation_mode_{OperationMode::NORMAL};
};

}  // namespace aidl::android::hardware::sensors::mainline
