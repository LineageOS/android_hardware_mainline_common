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

#include <cctype>
#include <cstring>

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

// Characters udev keeps verbatim in a device node name, see
// allow_listed_char_for_devnode() in systemd's src/basic/device-nodes.c.
bool IsAllowedChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isdigit(uc) || std::isalpha(uc)) {
        return true;
    }
    return std::strchr("#+-.:=@_", c) != nullptr && c != '\0';
}

// Additional characters allowed for rule input, UDEV_ALLOWED_CHARS_INPUT in
// systemd's src/udev/udev-format.h.
bool IsAllowedInputChar(char c) {
    return std::strchr("/ $%?,", c) != nullptr && c != '\0';
}

// Length of the UTF-8 sequence starting at `s`, or 0 if it is not a valid
// multi-byte sequence.
size_t Utf8SequenceLength(const std::string& s, size_t i) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    size_t length = 0;
    if ((lead & 0xe0) == 0xc0) {
        length = 2;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
    } else {
        return 0;
    }
    if (i + length > s.size()) {
        return 0;
    }
    for (size_t k = 1; k < length; k++) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) {
            return 0;
        }
    }
    return length;
}

/*
 * Reproduces what udev does to a sysfs attribute value substituted with
 * "$attr{...}" in a rule: udev_replace_chars(value, UDEV_ALLOWED_CHARS_INPUT)
 * in systemd's src/udev/udev-format.c. Every character that is neither
 * allow-listed, part of a "\x" escape nor part of a valid UTF-8 sequence is
 * replaced by '_'.
 *
 * hwdb entries are written from udev's output, so the lookup keys have to go
 * through the same transformation. Most importantly the device tree modalias
 * "of:NaccelerometerT(null)Csilan,sc7a20" becomes
 * "of:NaccelerometerT_null_Csilan,sc7a20".
 */
std::string UdevSanitize(const std::string& value) {
    std::string result = value;
    for (size_t i = 0; i < result.size();) {
        if (IsAllowedChar(result[i]) || IsAllowedInputChar(result[i])) {
            i++;
            continue;
        }
        if (result[i] == '\\' && i + 1 < result.size() && result[i + 1] == 'x') {
            i += 2;
            continue;
        }
        const size_t utf8_length = Utf8SequenceLength(result, i);
        if (utf8_length > 1) {
            i += utf8_length;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(result[i]))) {
            // ' ' is allow-listed for rule input, so whitespace becomes a
            // plain space instead of an underscore.
            result[i] = ' ';
        } else {
            result[i] = '_';
        }
        i++;
    }
    return result;
}

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

    // The rules substitute the DMI modalias with "$attr{[dmi/id]modalias}",
    // so it goes through the same sanitization as the device modalias.
    std::string dmi = UdevSanitize(ReadDmiModalias());
    if (dmi.empty()) {
        LOG(INFO) << "No DMI modalias available; only hwdb entries whose DMI part matches the "
                     "empty string (e.g. device tree entries ending in ':*') can match";
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
    // systemd's 60-sensor.rules always appends ":$attr{[dmi/id]modalias}",
    // so the match string ends with ':' on machines without DMI. Patterns of
    // device tree entries rely on it, e.g.
    // "sensor:modalias:of:NaccelerometerT_null_Csilan,sc7a20:*".
    const std::string sanitized = UdevSanitize(modalias);
    if (!label.empty()) {
        matches.push_back("sensor:" + UdevSanitize(label) + ":modalias:" + sanitized + ":" +
                          dmi_modalias_);
    }
    matches.push_back("sensor:modalias:" + sanitized + ":" + dmi_modalias_);
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
