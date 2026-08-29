/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsHwdb"

#include "SensorHwdb.h"

#include <sys/stat.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <libhwdb/Hwdb.h>

#include "smbios.h"

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr const char* kHwdbPaths[] = {
        "/odm/etc/hwdb.d/60-sensor.hwdb",
        "/vendor/etc/hwdb.d/60-sensor.hwdb",
};

void AppendFilteredAscii(std::string* output, const char* input) {
    if (input == nullptr) {
        return;
    }
    for (const char* p = input; *p != '\0'; ++p) {
        if (*p > ' ' && *p < 127 && *p != ':') {
            *output += *p;
        }
    }
}

std::string ReadSysfsString(const std::string& path) {
    std::string result;
    if (!::android::base::ReadFileToString(path, &result)) return "";
    if (result.empty()) return "";
    if (result.back() == '\0' || result.back() == '\x0a') result.pop_back();
    return ::android::base::Trim(result);
}

bool ReadDmiDataSysfs(const std::string& path, std::vector<uint8_t>* buffer) {
    std::string dmi_path = path + "/DMI";
    std::string entry_path = path + "/smbios_entry_point";

    struct stat info;
    if (stat(dmi_path.c_str(), &info) != 0) {
        return false;
    }

    std::string dmi_content = ReadSysfsString(dmi_path);
    std::string entry_content = ReadSysfsString(entry_path);
    if (dmi_content.empty() || entry_content.empty()) return false;

    size_t entry_size = std::min(entry_content.size(), static_cast<size_t>(32));
    buffer->resize(32 + dmi_content.size());
    std::memcpy(buffer->data(), entry_content.data(), entry_size);
    std::memcpy(buffer->data() + 32, dmi_content.data(), dmi_content.size());
    return true;
}

std::string BuildDmiFromSmbios() {
    std::vector<uint8_t> buffer;
    if (!ReadDmiDataSysfs("/sys/firmware/dmi/tables", &buffer)) {
        return "";
    }

    ParserContext parser;
    if (smbios_initialize(&parser, buffer.data(), buffer.size(), SMBIOS_ANY) != SMBERR_OK) {
        return "";
    }

    std::string bios_vendor;
    std::string bios_version;
    std::string bios_date;
    std::string system_vendor;
    std::string product_name;
    std::string product_version;
    std::string board_vendor;
    std::string board_name;
    std::string board_version;
    std::string chassis_vendor;
    std::string chassis_version;
    int chassis_type = -1;

    const Entry* entry = nullptr;
    while (smbios_next(&parser, &entry) == SMBERR_OK) {
        if (entry->type == TYPE_BIOS_INFO) {
            AppendFilteredAscii(&bios_vendor, entry->data.bios_info.Vendor);
            AppendFilteredAscii(&bios_version, entry->data.bios_info.BIOSVersion);
            AppendFilteredAscii(&bios_date, entry->data.bios_info.BIOSReleaseDate);
        } else if (entry->type == TYPE_SYSTEM_INFO) {
            AppendFilteredAscii(&system_vendor, entry->data.system_info.Manufacturer);
            AppendFilteredAscii(&product_name, entry->data.system_info.ProductName);
            AppendFilteredAscii(&product_version, entry->data.system_info.Version);
        } else if (entry->type == TYPE_BASEBOARD_INFO) {
            AppendFilteredAscii(&board_vendor, entry->data.baseboard_info.Manufacturer);
            AppendFilteredAscii(&board_name, entry->data.baseboard_info.Product);
            AppendFilteredAscii(&board_version, entry->data.baseboard_info.Version);
        } else if (entry->type == TYPE_SYSTEM_ENCLOSURE) {
            AppendFilteredAscii(&chassis_vendor, entry->data.system_enclosure.Manufacturer);
            chassis_type = entry->data.system_enclosure.Type & 0x7f;
            AppendFilteredAscii(&chassis_version, entry->data.system_enclosure.Version);
        }
    }

    std::string result = "dmi:";
    if (!bios_vendor.empty()) result += "bvn" + bios_vendor + ":";
    if (!bios_version.empty()) result += "bvr" + bios_version + ":";
    if (!bios_date.empty()) result += "bd" + bios_date + ":";
    if (!system_vendor.empty()) result += "svn" + system_vendor + ":";
    if (!product_name.empty()) result += "pn" + product_name + ":";
    if (!product_version.empty()) result += "pvr" + product_version + ":";
    if (!board_vendor.empty()) result += "rvn" + board_vendor + ":";
    if (!board_name.empty()) result += "rn" + board_name + ":";
    if (!board_version.empty()) result += "rvr" + board_version + ":";
    if (!chassis_vendor.empty()) result += "cvn" + chassis_vendor + ":";
    if (chassis_type != -1) result += "ct" + std::to_string(chassis_type) + ":";
    if (!chassis_version.empty()) result += "cvr" + chassis_version + ":";

    return result;
}

}  // namespace

class SensorHwdb::HwdbWrapper {
  public:
    std::unique_ptr<libhwdb::Hwdb> hwdb_;
    std::string dmi_modalias_;

    bool Initialize() {
        for (const auto* path : kHwdbPaths) {
            hwdb_ = libhwdb::Hwdb::FromFile(path);
            if (hwdb_) {
                LOG(INFO) << "Loaded sensor hwdb from " << path;
                break;
            }
        }

        if (!hwdb_) {
            LOG(WARNING) << "No sensor hwdb file found";
            return false;
        }

        dmi_modalias_ = SensorHwdb::GetDmiModalias();
        if (dmi_modalias_.empty()) {
            LOG(WARNING) << "Failed to get DMI modalias";
        } else {
            LOG(INFO) << "DMI modalias: " << dmi_modalias_;
        }

        return true;
    }
};

SensorHwdb::SensorHwdb() = default;

SensorHwdb::~SensorHwdb() = default;

std::unique_ptr<SensorHwdb> SensorHwdb::Create() {
    std::unique_ptr<SensorHwdb> instance(new SensorHwdb());
    instance->hwdb_ = std::make_unique<HwdbWrapper>();

    if (!instance->hwdb_->Initialize()) {
        return nullptr;
    }

    return instance;
}

std::vector<std::string> SensorHwdb::BuildQueryStrings(const std::string& device_modalias,
                                                        const std::string& label) const {
    std::vector<std::string> queries;
    const std::string& dmi = hwdb_->dmi_modalias_;

    if (!label.empty()) {
        std::string query = "sensor:" + label + ":modalias:" + device_modalias;
        if (!dmi.empty() && query.back() != ':' && dmi.front() != ':') {
            query += ":";
        }
        query += dmi;
        queries.push_back(query);
    }

    std::string query = "sensor:modalias:" + device_modalias;
    if (!dmi.empty() && query.back() != ':' && dmi.front() != ':') {
        query += ":";
    }
    query += dmi;
    queries.push_back(query);

    return queries;
}

bool SensorHwdb::ParseMountMatrixFromString(const std::string& content, float matrix[9]) const {
    auto rows = ::android::base::Split(content, ";");
    if (rows.size() != 3) {
        return false;
    }

    float parsed[9];
    for (int r = 0; r < 3; r++) {
        auto cols = ::android::base::Split(rows[r], ",");
        if (cols.size() != 3) {
            return false;
        }
        for (int c = 0; c < 3; c++) {
            std::string trimmed = ::android::base::Trim(cols[c]);
            char* end = nullptr;
            float val = std::strtof(trimmed.c_str(), &end);
            if (end == trimmed.c_str() || *end != '\0') {
                return false;
            }
            parsed[r * 3 + c] = val;
        }
    }

    if (!std::any_of(parsed, parsed + 9, [](float v) { return v != 0.0f; })) {
        return false;
    }

    std::copy(parsed, parsed + 9, matrix);
    return true;
}

bool SensorHwdb::GetMountMatrix(const std::string& device_modalias, const std::string& label,
                                 float matrix[9]) const {
    if (!hwdb_ || !hwdb_->hwdb_) {
        return false;
    }

    auto queries = BuildQueryStrings(device_modalias, label);
    auto props = hwdb_->hwdb_->GetProperties(queries);

    auto it = props.find("ACCEL_MOUNT_MATRIX");
    if (it == props.end()) {
        return false;
    }

    if (!ParseMountMatrixFromString(it->second, matrix)) {
        LOG(WARNING) << "Invalid ACCEL_MOUNT_MATRIX from hwdb: " << it->second;
        return false;
    }

    LOG(INFO) << "Mount matrix from hwdb for " << device_modalias << ": " << it->second;
    return true;
}

int SensorHwdb::GetProximityNearLevel(const std::string& device_modalias, const std::string& label,
                                      int default_value) const {
    if (!hwdb_ || !hwdb_->hwdb_) {
        return default_value;
    }

    auto queries = BuildQueryStrings(device_modalias, label);
    auto props = hwdb_->hwdb_->GetProperties(queries);

    auto it = props.find("PROXIMITY_NEAR_LEVEL");
    if (it == props.end()) {
        return default_value;
    }

    char* end = nullptr;
    long val = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || *end != '\0') {
        LOG(WARNING) << "Invalid PROXIMITY_NEAR_LEVEL from hwdb: " << it->second;
        return default_value;
    }

    LOG(INFO) << "Proximity near level from hwdb for " << device_modalias << ": " << val;
    return static_cast<int>(val);
}

std::map<std::string, std::string> SensorHwdb::GetSensorProperties(
        const std::string& device_modalias, const std::string& label) const {
    if (!hwdb_ || !hwdb_->hwdb_) {
        return {};
    }

    auto queries = BuildQueryStrings(device_modalias, label);
    return hwdb_->hwdb_->GetProperties(queries);
}

std::string SensorHwdb::GetDmiModalias() {
    std::string dmi_modalias = ReadSysfsString("/sys/class/dmi/id/modalias");

    if (dmi_modalias.empty()) {
        LOG(INFO) << "DMI modalias not available from sysfs, trying SMBIOS fallback";
        dmi_modalias = BuildDmiFromSmbios();
    }

    if (dmi_modalias.empty()) {
        LOG(WARNING) << "Failed to get DMI modalias from both sysfs and SMBIOS";
    }

    return dmi_modalias;
}

}  // namespace aidl::android::hardware::sensors::mainline
