/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsWorker"

#include "libsensors_common/PeriodicWorker.h"

#include <android-base/logging.h>

#include <pthread.h>

#include <chrono>

namespace aidl::android::hardware::sensors::mainline {

namespace {
constexpr int64_t kMinPeriodNs = 1000 * 1000;  // 1 ms
}  // namespace

PeriodicWorker::PeriodicWorker(std::string name, Task task)
    : name_(std::move(name)), task_(std::move(task)) {}

PeriodicWorker::~PeriodicWorker() {
    Stop();
}

void PeriodicWorker::Start(int64_t period_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    period_ns_ = std::max(period_ns, kMinPeriodNs);
    if (running_) {
        cv_.notify_all();
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = true;
    stop_requested_ = false;
    poke_requested_ = false;
    thread_ = std::thread(&PeriodicWorker::Run, this);
    LOG(DEBUG) << "Worker '" << name_ << "' started, period " << period_ns_ / 1000 << " us";
}

void PeriodicWorker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !thread_.joinable()) {
            return;
        }
        stop_requested_ = true;
        cv_.notify_all();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    LOG(DEBUG) << "Worker '" << name_ << "' stopped";
}

void PeriodicWorker::SetPeriod(int64_t period_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    period_ns_ = std::max(period_ns, kMinPeriodNs);
    cv_.notify_all();
}

void PeriodicWorker::Poke() {
    std::lock_guard<std::mutex> lock(mutex_);
    poke_requested_ = true;
    cv_.notify_all();
}

bool PeriodicWorker::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

void PeriodicWorker::Run() {
    pthread_setname_np(pthread_self(), name_.substr(0, 15).c_str());

    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_requested_) {
        const auto period = std::chrono::nanoseconds(period_ns_);
        const auto deadline = std::chrono::steady_clock::now() + period;
        poke_requested_ = false;

        lock.unlock();
        task_();
        lock.lock();

        while (!stop_requested_ && !poke_requested_) {
            // Re-read the period in case it changed while the task was running
            // and compute the deadline from the start of this iteration.
            const auto effective_deadline =
                    deadline - period + std::chrono::nanoseconds(period_ns_);
            if (cv_.wait_until(lock, effective_deadline) == std::cv_status::timeout) {
                break;
            }
            if (std::chrono::steady_clock::now() >= effective_deadline) {
                break;
            }
        }
    }
    running_ = false;
}

}  // namespace aidl::android::hardware::sensors::mainline
