/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <string>
#include <thread>

#include <android-base/unique_fd.h>

namespace aidl::android::hardware::camera::mainline {

// Watches a device directory for V4L2 / media controller nodes coming and
// going and runs `rescan` after the changes settled down.
//
// `rescan` returns true when it wants to run again a little later (a node
// could not be opened yet). Owner, mode and SELinux label changes by ueventd
// also trigger a rescan, the retry is the fallback when those don't help.
class DeviceMonitor {
  public:
    using RescanFunction = std::function<bool()>;

    DeviceMonitor(std::string dev_dir, RescanFunction rescan);
    ~DeviceMonitor();

    DeviceMonitor(const DeviceMonitor&) = delete;
    DeviceMonitor& operator=(const DeviceMonitor&) = delete;

    // Starts the monitoring thread. Returns false if the directory can not be
    // watched; hotplug is then not supported, but everything else works.
    bool Start();

  private:
    void Run();

    const std::string dev_dir_;
    const RescanFunction rescan_;
    ::android::base::unique_fd inotify_fd_;
    ::android::base::unique_fd stop_fd_;
    std::thread thread_;
};

}  // namespace aidl::android::hardware::camera::mainline
