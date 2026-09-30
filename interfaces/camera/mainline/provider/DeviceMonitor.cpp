/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_DeviceMonitor"

#include "provider/DeviceMonitor.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <chrono>
#include <cstring>

#include <android-base/logging.h>
#include <android-base/strings.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Time without further events before a rescan, so that the burst of nodes a
// device creates (and ueventd's permission changes) is handled in one go.
constexpr milliseconds kSettleTime{150};
// Retries for nodes that could not be opened yet.
constexpr milliseconds kRetryDelay{250};
constexpr int kMaxRetries = 20;

bool IsRelevantNode(const char* name) {
    return ::android::base::StartsWith(name, "video") ||
           ::android::base::StartsWith(name, "media") ||
           ::android::base::StartsWith(name, "v4l-subdev");
}

}  // namespace

DeviceMonitor::DeviceMonitor(std::string dev_dir, RescanFunction rescan)
    : dev_dir_(std::move(dev_dir)), rescan_(std::move(rescan)) {}

DeviceMonitor::~DeviceMonitor() {
    if (thread_.joinable()) {
        const uint64_t value = 1;
        if (TEMP_FAILURE_RETRY(write(stop_fd_.get(), &value, sizeof(value))) < 0) {
            PLOG(ERROR) << "failed to stop the monitor thread";
        }
        thread_.join();
    }
}

bool DeviceMonitor::Start() {
    inotify_fd_.reset(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    if (inotify_fd_.get() < 0) {
        PLOG(ERROR) << "inotify_init1";
        return false;
    }
    if (inotify_add_watch(inotify_fd_.get(), dev_dir_.c_str(),
                          IN_CREATE | IN_DELETE | IN_ATTRIB | IN_MOVED_TO | IN_MOVED_FROM) < 0) {
        PLOG(ERROR) << "failed to watch " << dev_dir_ << ", hotplug disabled";
        return false;
    }
    stop_fd_.reset(eventfd(0, EFD_CLOEXEC));
    if (stop_fd_.get() < 0) {
        PLOG(ERROR) << "eventfd";
        return false;
    }
    thread_ = std::thread([this] { Run(); });
    LOG(INFO) << "watching " << dev_dir_ << " for camera hotplug";
    return true;
}

void DeviceMonitor::Run() {
    bool pending = false;
    int retries = 0;
    Clock::time_point deadline;

    while (true) {
        int timeout_ms = -1;
        if (pending) {
            const auto remaining =
                    std::chrono::duration_cast<milliseconds>(deadline - Clock::now()).count();
            timeout_ms = remaining > 0 ? static_cast<int>(remaining) : 0;
        }

        pollfd fds[] = {
                {.fd = inotify_fd_.get(), .events = POLLIN, .revents = 0},
                {.fd = stop_fd_.get(), .events = POLLIN, .revents = 0},
        };
        const int ret = TEMP_FAILURE_RETRY(poll(fds, 2, timeout_ms));
        if (ret < 0) {
            PLOG(ERROR) << "poll, hotplug disabled";
            return;
        }
        if (fds[1].revents & POLLIN) return;

        if (fds[0].revents & POLLIN) {
            alignas(inotify_event) char buffer[4096];
            ssize_t length;
            while ((length = read(inotify_fd_.get(), buffer, sizeof(buffer))) > 0) {
                for (ssize_t offset = 0; offset < length;) {
                    const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
                    offset += sizeof(inotify_event) + event->len;
                    if (event->mask & IN_Q_OVERFLOW) {
                        // Events were lost, rescan to be safe.
                        pending = true;
                    } else if (event->len > 0 && IsRelevantNode(event->name)) {
                        LOG(VERBOSE)
                                << "event 0x" << std::hex << event->mask << " on " << event->name;
                        pending = true;
                    }
                }
            }
            if (pending) {
                deadline = Clock::now() + kSettleTime;
                retries = 0;
            }
        }

        if (pending && Clock::now() >= deadline) {
            pending = false;
            if (rescan_()) {
                if (retries < kMaxRetries) {
                    ++retries;
                    pending = true;
                    deadline = Clock::now() + kRetryDelay;
                } else {
                    LOG(WARNING) << "giving up on nodes that can not be opened";
                }
            }
        }
    }
}

}  // namespace aidl::android::hardware::camera::mainline
