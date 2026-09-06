/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsHwdb"

#include "libsensors_hwdb/SensorHwdb.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <libhwdb/Hwdb.h>

#include "DmiModalias.h"
#include "libsensors_common/Sysfs.h"

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr const char* kHwdbDirs[] = {
        "/vendor/etc/sensors/hwdb.d",
        "/odm/etc/sensors/hwdb.d",
};

constexpr const char* kLegacyHwdbFiles[] = {
        "/vendor/etc/hwdb.d/60-sensor.hwdb",
        "/odm/etc/hwdb.d/60-sensor.hwdb",
};

std::vector<std::string> FindHwdbFiles() {
    std::vector<std::string> files;
    for (const char* dir : kHwdbDirs) {
        for (const auto& entry : sysfs::ListDirectory(dir)) {
            if (::android::base::EndsWith(entry, ".hwdb")) {
                files.push_back(std::string(dir) + "/" + entry);
            }
        }
    }
    for (const char* path : kLegacyHwdbFiles) {
        if (sysfs::Exists(path)) {
            files.push_back(path);
        }
    }
    return files;
}

}  // namespace

SensorHwdb::SensorHwdb(std::unique_ptr<libhwdb::Hwdb> hwdb, std::string dmi_modalias)
    : hwdb_(std::move(hwdb)), dmi_modalias_(std::move(dmi_modalias)) {}

SensorHwdb::~SensorHwdb() = default;

std::unique_ptr<SensorHwdb> SensorHwdb::Load() {
    std::vector<std::string> files = FindHwdbFiles();
    if (files.empty()) {
        LOG(INFO) << "No sensor hwdb file found";
        return nullptr;
    }
    return LoadFiles(files);
}

std::unique_ptr<SensorHwdb> SensorHwdb::LoadFiles(const std::vector<std::string>& paths) {
    // Later files take precedence: libhwdb applies matching entries in file
    // order, so the last matching entry wins for a given property.
    std::string merged;
    for (const auto& path : paths) {
        std::string content;
        if (!::android::base::ReadFileToString(path, &content)) {
            LOG(WARNING) << "Cannot read hwdb file " << path;
            continue;
        }
        LOG(INFO) << "Loading sensor hwdb file " << path << " (" << content.size() << " bytes)";
        merged += content;
        merged += "\n\n";
    }
    if (merged.empty()) {
        return nullptr;
    }

    std::unique_ptr<libhwdb::Hwdb> hwdb = libhwdb::Hwdb::FromContent(merged);
    if (!hwdb) {
        LOG(WARNING) << "Failed to parse sensor hwdb content";
        return nullptr;
    }

    std::string dmi = ReadDmiModalias();
    if (dmi.empty()) {
        LOG(INFO) << "No DMI modalias available; only device tree/ACPI style hwdb entries "
                     "without DMI part can match";
    } else {
        LOG(INFO) << "DMI modalias: " << dmi;
    }
    return std::unique_ptr<SensorHwdb>(new SensorHwdb(std::move(hwdb), std::move(dmi)));
}

std::string SensorHwdb::ReadDmiModalias() {
    std::string modalias = sysfs::ReadString("/sys/class/dmi/id/modalias", "");
    if (!modalias.empty()) {
        return modalias;
    }
    if (!sysfs::Exists("/sys/firmware/dmi/tables/DMI")) {
        return "";
    }
    LOG(INFO) << "/sys/class/dmi/id/modalias missing, decoding SMBIOS tables instead";
    return BuildDmiModaliasFromSmbios();
}

std::vector<std::string> SensorHwdb::BuildMatchStrings(const std::string& modalias,
                                                       const std::string& label) const {
    std::vector<std::string> matches;
    if (modalias.empty()) {
        return matches;
    }
    if (!label.empty()) {
        if (!dmi_modalias_.empty()) {
            matches.push_back("sensor:" + label + ":modalias:" + modalias + ":" + dmi_modalias_);
        }
        matches.push_back("sensor:" + label + ":modalias:" + modalias);
    }
    if (!dmi_modalias_.empty()) {
        matches.push_back("sensor:modalias:" + modalias + ":" + dmi_modalias_);
    }
    matches.push_back("sensor:modalias:" + modalias);
    return matches;
}

std::map<std::string, std::string> SensorHwdb::Lookup(const std::string& modalias,
                                                      const std::string& label) const {
    std::map<std::string, std::string> result;
    for (const auto& match : BuildMatchStrings(modalias, label)) {
        std::map<std::string, std::string> props = hwdb_->GetProperties(match);
        for (const auto& [key, value] : props) {
            // The first (most specific) match string wins.
            result.emplace(key, value);
        }
        if (!props.empty()) {
            LOG(DEBUG) << "hwdb match '" << match << "': " << props.size() << " propert(y/ies)";
        }
    }
    return result;
}

std::optional<std::string> SensorHwdb::GetMountMatrix(const std::string& modalias,
                                                      const std::string& label) const {
    auto props = Lookup(modalias, label);
    auto it = props.find(kAccelMountMatrix);
    if (it == props.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<int64_t> SensorHwdb::GetProximityNearLevel(const std::string& modalias,
                                                         const std::string& label) const {
    auto props = Lookup(modalias, label);
    auto it = props.find(kProximityNearLevel);
    if (it == props.end()) {
        return std::nullopt;
    }
    int64_t value = 0;
    if (!::android::base::ParseInt(it->second.c_str(), &value)) {
        LOG(WARNING) << "Invalid " << kProximityNearLevel << " in hwdb: '" << it->second << "'";
        return std::nullopt;
    }
    return value;
}

}  // namespace aidl::android::hardware::sensors::mainline
