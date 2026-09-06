/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace aidl::android::hardware::sensors::mainline {

/*
 * Runs a task on a dedicated thread at a fixed period. Used by backends that
 * have to poll sysfs attributes or generate data on their own.
 *
 * The task is run once immediately after Start() and then every period. The
 * period can be changed at any time; the change takes effect immediately.
 */
class PeriodicWorker {
  public:
    using Task = std::function<void()>;

    // `name` is used as the thread name (truncated to 15 characters).
    PeriodicWorker(std::string name, Task task);
    ~PeriodicWorker();

    PeriodicWorker(const PeriodicWorker&) = delete;
    PeriodicWorker& operator=(const PeriodicWorker&) = delete;

    // Starts the worker thread. No-op if already running.
    void Start(int64_t period_ns);

    // Stops the worker thread and waits for it to exit. No-op if not running.
    void Stop();

    // Updates the period and wakes the thread so the new period applies at
    // once.
    void SetPeriod(int64_t period_ns);

    // Wakes the thread to run the task right away.
    void Poke();

    bool IsRunning() const;

  private:
    void Run();

    const std::string name_;
    const Task task_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool running_ = false;
    bool stop_requested_ = false;
    bool poke_requested_ = false;
    int64_t period_ns_ = 0;
};

}  // namespace aidl::android::hardware::sensors::mainline
