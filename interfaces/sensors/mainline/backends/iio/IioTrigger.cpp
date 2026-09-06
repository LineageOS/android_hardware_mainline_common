/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsIio"

#include "IioTrigger.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <libsensors_common/Sysfs.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace aidl::android::hardware::sensors::mainline {

namespace {
constexpr const char* kIioDevicesDir = "/sys/bus/iio/devices";
constexpr const char* kHrtimerConfigfsDir = "/config/iio/triggers/hrtimer";
}  // namespace

IioTrigger::IioTrigger(std::string device_sysfs_path, std::string name, bool owns_hrtimer)
    : device_sysfs_path_(std::move(device_sysfs_path)),
      name_(std::move(name)),
      owns_hrtimer_(owns_hrtimer) {
    auto dir = FindTriggerSysfsDir(name_);
    if (dir.has_value() && sysfs::Exists(*dir + "/sampling_frequency")) {
        frequency_attr_ = *dir + "/sampling_frequency";
    }
}

IioTrigger::~IioTrigger() {
    // Detach from the device first, otherwise the configfs directory cannot
    // be removed.
    WriteCurrentTrigger(device_sysfs_path_, "");
    if (owns_hrtimer_) {
        std::string path = std::string(kHrtimerConfigfsDir) + "/" + name_;
        if (rmdir(path.c_str()) == 0) {
            LOG(INFO) << "Removed hrtimer trigger " << name_;
        } else {
            LOG(WARNING) << "Failed to remove hrtimer trigger " << path << ": " << strerror(errno);
        }
    } else {
        LOG(INFO) << "Detached trigger " << name_ << " from " << device_sysfs_path_;
    }
}

bool IioTrigger::DeviceHasTriggerInterface(const std::string& device_sysfs_path) {
    return sysfs::Exists(device_sysfs_path + "/trigger/current_trigger");
}

bool IioTrigger::WriteCurrentTrigger(const std::string& device_sysfs_path,
                                     const std::string& trigger_name) {
    // The kernel treats an empty write as "no trigger"; a trailing newline is
    // stripped by the kernel.
    return sysfs::WriteString(device_sysfs_path + "/trigger/current_trigger", trigger_name + "\n");
}

std::optional<std::string> IioTrigger::FindTriggerSysfsDir(const std::string& trigger_name) {
    for (const auto& entry : sysfs::ListDirectory(kIioDevicesDir)) {
        if (!::android::base::StartsWith(entry, "trigger")) {
            continue;
        }
        std::string dir = std::string(kIioDevicesDir) + "/" + entry;
        if (sysfs::ReadString(dir + "/name", "") == trigger_name) {
            return dir;
        }
    }
    return std::nullopt;
}

std::optional<std::string> IioTrigger::FindDeviceTrigger(int dev_num,
                                                         const std::string& device_name) {
    // Naming conventions used by in-kernel drivers, most specific first.
    const std::string dev_suffix = "-dev" + std::to_string(dev_num);
    std::vector<std::string> exact = {
            device_name + dev_suffix,  // bmi160-dev0, accel_3d-dev1, ...
            device_name + "-trigger",  // st_sensors: lsm303dlhc_accel-trigger
    };

    std::vector<std::pair<std::string, std::string>> candidates;  // (name, dir)
    for (const auto& entry : sysfs::ListDirectory(kIioDevicesDir)) {
        if (!::android::base::StartsWith(entry, "trigger")) {
            continue;
        }
        std::string dir = std::string(kIioDevicesDir) + "/" + entry;
        std::string name = sysfs::ReadString(dir + "/name", "");
        if (name.empty() || ::android::base::StartsWith(name, kHrtimerPrefix)) {
            continue;
        }
        candidates.emplace_back(name, dir);
    }

    for (const auto& wanted : exact) {
        for (const auto& [name, dir] : candidates) {
            if (name == wanted) {
                return name;
            }
        }
    }
    for (const auto& [name, dir] : candidates) {
        // bmi270-trig-<irq>, kionix data-rdy-dev<N>, opt4060 data-ready-dev<N>
        if (::android::base::StartsWith(name, device_name) &&
            (name.find("-trig") != std::string::npos || name.find("rdy") != std::string::npos ||
             name.find("ready") != std::string::npos) &&
            (name.find("any-motion") == std::string::npos)) {
            // A "-dev<N>" suffix must match our device if present.
            size_t dev = name.find("-dev");
            if (dev == std::string::npos || ::android::base::EndsWith(name, dev_suffix)) {
                return name;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> IioTrigger::CreateHrtimerTrigger(int dev_num) {
    if (!sysfs::IsDirectory(kHrtimerConfigfsDir)) {
        LOG(INFO) << kHrtimerConfigfsDir
                  << " not available (CONFIG_IIO_CONFIGFS/CONFIG_IIO_HRTIMER_TRIGGER disabled or "
                     "configfs not mounted)";
        return std::nullopt;
    }
    std::string name = std::string(kHrtimerPrefix) + std::to_string(dev_num);
    std::string path = std::string(kHrtimerConfigfsDir) + "/" + name;
    if (!sysfs::IsDirectory(path)) {
        if (mkdir(path.c_str(), 0755) != 0) {
            LOG(WARNING) << "Failed to create hrtimer trigger " << path << ": " << strerror(errno);
            return std::nullopt;
        }
    }
    if (!FindTriggerSysfsDir(name).has_value()) {
        LOG(WARNING) << "hrtimer trigger " << name << " created but not visible in "
                     << kIioDevicesDir;
        rmdir(path.c_str());
        return std::nullopt;
    }
    LOG(INFO) << "Created hrtimer trigger " << name;
    return name;
}

std::unique_ptr<IioTrigger> IioTrigger::Assign(const std::string& device_sysfs_path, int dev_num,
                                               const std::string& device_name) {
    const std::string current_path = device_sysfs_path + "/trigger/current_trigger";
    if (!sysfs::Exists(current_path)) {
        return nullptr;
    }

    // 1. Already assigned (by the driver, e.g. HID sensors, or by someone else).
    std::string current = sysfs::ReadString(current_path, "");
    if (!current.empty() && !::android::base::StartsWith(current, kHrtimerPrefix)) {
        LOG(INFO) << "Device " << dev_num << " already uses trigger '" << current << "'";
        return std::unique_ptr<IioTrigger>(new IioTrigger(device_sysfs_path, current, false));
    }

    // 2. Trigger provided by the driver of this device.
    auto device_trigger = FindDeviceTrigger(dev_num, device_name);
    if (device_trigger.has_value()) {
        if (WriteCurrentTrigger(device_sysfs_path, *device_trigger)) {
            LOG(INFO) << "Device " << dev_num << " uses device trigger '" << *device_trigger << "'";
            return std::unique_ptr<IioTrigger>(
                    new IioTrigger(device_sysfs_path, *device_trigger, false));
        }
        LOG(WARNING) << "Failed to assign device trigger '" << *device_trigger << "' to device "
                     << dev_num;
    }

    // 3. hrtimer software trigger.
    auto hrtimer = CreateHrtimerTrigger(dev_num);
    if (hrtimer.has_value()) {
        if (WriteCurrentTrigger(device_sysfs_path, *hrtimer)) {
            LOG(INFO) << "Device " << dev_num << " uses hrtimer trigger '" << *hrtimer << "'";
            return std::unique_ptr<IioTrigger>(new IioTrigger(device_sysfs_path, *hrtimer, true));
        }
        LOG(WARNING) << "Failed to assign hrtimer trigger '" << *hrtimer << "' to device "
                     << dev_num << ": " << strerror(errno);
        rmdir((std::string(kHrtimerConfigfsDir) + "/" + *hrtimer).c_str());
    }

    LOG(WARNING) << "No trigger available for IIO device " << dev_num << " ('" << device_name
                 << "')";
    return nullptr;
}

bool IioTrigger::SetFrequency(double hz) {
    if (frequency_attr_.empty()) {
        return false;
    }
    // The hrtimer trigger accepts milli-hertz precision and rejects 0.
    if (hz < 0.001) {
        hz = 0.001;
    }
    bool ok = sysfs::WriteDouble(frequency_attr_, hz);
    LOG(DEBUG) << "Trigger " << name_ << " frequency " << hz << " Hz -> " << (ok ? "ok" : "failed");
    return ok;
}

void IioTrigger::CleanupStaleTriggers() {
    if (!sysfs::IsDirectory(kHrtimerConfigfsDir)) {
        return;
    }
    for (const auto& entry : sysfs::ListDirectory(kHrtimerConfigfsDir)) {
        if (!::android::base::StartsWith(entry, kHrtimerPrefix)) {
            continue;
        }
        // Detach it from any device still referencing it.
        for (const auto& dev : sysfs::ListDirectory(kIioDevicesDir)) {
            if (!::android::base::StartsWith(dev, "iio:device")) {
                continue;
            }
            std::string dev_path = std::string(kIioDevicesDir) + "/" + dev;
            if (sysfs::ReadString(dev_path + "/trigger/current_trigger", "") == entry) {
                WriteCurrentTrigger(dev_path, "");
            }
        }
        std::string path = std::string(kHrtimerConfigfsDir) + "/" + entry;
        if (rmdir(path.c_str()) == 0) {
            LOG(INFO) << "Removed stale hrtimer trigger " << entry;
        } else {
            LOG(WARNING) << "Failed to remove stale hrtimer trigger " << path << ": "
                         << strerror(errno);
        }
    }
}

}  // namespace aidl::android::hardware::sensors::mainline
