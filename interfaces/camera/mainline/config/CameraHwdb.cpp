/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Hwdb"

#include "config/CameraHwdb.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <libhwdb/Hwdb.h>
#include <libhwdb/UdevSanitize.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

constexpr const char* kHwdbDirs[] = {
        "/vendor/etc/camera/hwdb.d",
        "/odm/etc/camera/hwdb.d",
};

constexpr const char* kLegacyHwdbFiles[] = {
        "/vendor/etc/hwdb.d/70-cameras.hwdb",
        "/odm/etc/hwdb.d/70-cameras.hwdb",
};

std::vector<std::string> FindHwdbFiles() {
    std::vector<std::string> files;
    for (const char* dir : kHwdbDirs) {
        std::unique_ptr<DIR, decltype(&closedir)> handle(opendir(dir), closedir);
        if (handle == nullptr) continue;
        std::vector<std::string> names;
        while (const dirent* entry = readdir(handle.get())) {
            if (::android::base::EndsWith(entry->d_name, ".hwdb")) names.push_back(entry->d_name);
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) files.push_back(std::string(dir) + "/" + name);
    }
    for (const char* path : kLegacyHwdbFiles) {
        if (access(path, R_OK) == 0) files.push_back(path);
    }
    return files;
}

}  // namespace

CameraHwdb::CameraHwdb(std::unique_ptr<libhwdb::Hwdb> hwdb) : hwdb_(std::move(hwdb)) {}

CameraHwdb::~CameraHwdb() = default;

std::unique_ptr<CameraHwdb> CameraHwdb::Load() {
    // libhwdb applies matching entries in order, so for a property the last
    // file that sets it wins.
    std::string merged;
    for (const auto& path : FindHwdbFiles()) {
        std::string content;
        if (!::android::base::ReadFileToString(path, &content)) {
            LOG(WARNING) << "cannot read " << path;
            continue;
        }
        LOG(INFO) << "loading camera hwdb " << path;
        merged += content;
        merged += "\n\n";
    }
    if (merged.empty()) {
        LOG(INFO) << "no camera hwdb";
        return nullptr;
    }
    return FromContent(merged);
}

std::unique_ptr<CameraHwdb> CameraHwdb::FromContent(const std::string& content) {
    auto hwdb = libhwdb::Hwdb::FromContent(content);
    if (hwdb == nullptr) {
        LOG(WARNING) << "failed to parse the camera hwdb";
        return nullptr;
    }
    return std::unique_ptr<CameraHwdb>(new CameraHwdb(std::move(hwdb)));
}

std::string CameraHwdb::MatchKey(uint16_t vendor_id, uint16_t product_id, const std::string& name) {
    // As built by systemd's 70-camera.rules from ID_VENDOR_ID, ID_MODEL_ID
    // (lower case hex) and $attr{name}.
    return ::android::base::StringPrintf("camera:usb:v%04xp%04x:name:%s:", vendor_id, product_id,
                                         libhwdb::UdevSanitize(name).c_str());
}

CameraHwdb::Entry CameraHwdb::Lookup(uint16_t vendor_id, uint16_t product_id,
                                     const std::string& name) const {
    Entry entry;
    const std::string key = MatchKey(vendor_id, product_id, name);
    const auto properties = hwdb_->GetProperties(key);
    if (auto it = properties.find("ID_CAMERA_DIRECTION"); it != properties.end()) {
        if (it->second == "front") {
            entry.direction = Facing::kFront;
        } else if (it->second == "rear") {
            entry.direction = Facing::kBack;
        }
    }
    if (auto it = properties.find("ID_INFRARED_CAMERA"); it != properties.end()) {
        entry.infrared = it->second == "1";
    }
    if (!properties.empty()) {
        LOG(DEBUG) << key << ": " << properties.size() << " hwdb propert"
                   << (properties.size() == 1 ? "y" : "ies");
    }
    return entry;
}

}  // namespace aidl::android::hardware::camera::mainline
