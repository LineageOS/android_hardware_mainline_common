/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsHwdb"

#include "DmiModalias.h"

#include <android-base/file.h>
#include <android-base/logging.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "smbios.h"

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr const char* kDmiTablesDir = "/sys/firmware/dmi/tables";
// libsmbios_parser expects a 32 byte entry point header followed by the table.
constexpr size_t kEntryPointSize = 32;

// Mirrors the kernel's dmi_id.c: only printable ASCII, no ':' or spaces.
void AppendFiltered(std::string* out, const char* prefix, const char* value) {
    if (value == nullptr || *value == '\0') {
        return;
    }
    std::string filtered;
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p > ' ' && *p < 127 && *p != ':') {
            filtered += *p;
        }
    }
    *out += prefix;
    *out += filtered;
    *out += ':';
}

bool ReadRawTables(std::vector<uint8_t>* buffer) {
    std::string entry_point;
    std::string table;
    if (!::android::base::ReadFileToString(std::string(kDmiTablesDir) + "/smbios_entry_point",
                                           &entry_point) ||
        !::android::base::ReadFileToString(std::string(kDmiTablesDir) + "/DMI", &table)) {
        return false;
    }
    if (entry_point.empty() || table.empty()) {
        return false;
    }
    buffer->assign(kEntryPointSize, 0);
    std::memcpy(buffer->data(), entry_point.data(), std::min(entry_point.size(), kEntryPointSize));
    buffer->insert(buffer->end(), table.begin(), table.end());
    return true;
}

// Mirrors dmi_save_release() in the kernel's drivers/firmware/dmi_scan.c:
// "<major>.<minor>", not reported when the BIOS says 0xff/0xff.
void AppendRelease(std::string* out, const char* prefix, uint8_t major, uint8_t minor) {
    if (major == 0xff && minor == 0xff) {
        return;
    }
    *out += prefix;
    *out += std::to_string(major);
    *out += '.';
    *out += std::to_string(minor);
    *out += ':';
}

}  // namespace

std::string BuildDmiModaliasFromSmbios() {
    std::vector<uint8_t> buffer;
    if (!ReadRawTables(&buffer)) {
        LOG(DEBUG) << "SMBIOS tables not readable from " << kDmiTablesDir;
        return "";
    }

    ParserContext parser;
    int ret = smbios_initialize(&parser, buffer.data(), buffer.size(), SMBIOS_ANY);
    if (ret != SMBERR_OK) {
        LOG(WARNING) << "Failed to parse SMBIOS tables: " << ret;
        return "";
    }

    std::string bios_vendor, bios_version, bios_date, bios_release, ec_release;
    std::string sys_vendor, product_name, product_version, product_sku, product_family;
    std::string board_vendor, board_name, board_version;
    std::string chassis_vendor, chassis_version;
    int chassis_type = -1;

    const Entry* entry = nullptr;
    while (smbios_next(&parser, &entry) == SMBERR_OK) {
        switch (entry->type) {
            case TYPE_BIOS_INFO:
                AppendFiltered(&bios_vendor, "bvn", entry->data.bios_info.Vendor);
                AppendFiltered(&bios_version, "bvr", entry->data.bios_info.BIOSVersion);
                AppendFiltered(&bios_date, "bd", entry->data.bios_info.BIOSReleaseDate);
                // The kernel only reports these when the structure is long
                // enough to hold them.
                if (entry->length >= 21) {
                    AppendRelease(&bios_release, "br", entry->data.bios_info.SystemBIOSMajorRelease,
                                  entry->data.bios_info.SystemBIOSMinorRelease);
                }
                if (entry->length >= 23) {
                    AppendRelease(&ec_release, "efr",
                                  entry->data.bios_info.EmbeddedControlerFirmwareMajorRelease,
                                  entry->data.bios_info.EmbeddedControlerFirmwareMinorRelease);
                }
                break;
            case TYPE_SYSTEM_INFO:
                AppendFiltered(&sys_vendor, "svn", entry->data.system_info.Manufacturer);
                AppendFiltered(&product_name, "pn", entry->data.system_info.ProductName);
                AppendFiltered(&product_version, "pvr", entry->data.system_info.Version);
                if (entry->length > 25) {
                    AppendFiltered(&product_sku, "sku", entry->data.system_info.SKUNumber);
                }
                if (entry->length > 26) {
                    AppendFiltered(&product_family, "pfa", entry->data.system_info.Family);
                }
                break;
            case TYPE_BASEBOARD_INFO:
                AppendFiltered(&board_vendor, "rvn", entry->data.baseboard_info.Manufacturer);
                AppendFiltered(&board_name, "rn", entry->data.baseboard_info.Product);
                AppendFiltered(&board_version, "rvr", entry->data.baseboard_info.Version);
                break;
            case TYPE_SYSTEM_ENCLOSURE:
                AppendFiltered(&chassis_vendor, "cvn", entry->data.system_enclosure.Manufacturer);
                chassis_type = entry->data.system_enclosure.Type & 0x7f;
                AppendFiltered(&chassis_version, "cvr", entry->data.system_enclosure.Version);
                break;
            default:
                break;
        }
    }

    // Field order of get_modalias() in the kernel's drivers/firmware/dmi-id.c.
    std::string modalias = "dmi:";
    modalias += bios_vendor + bios_version + bios_date + bios_release + ec_release;
    modalias += sys_vendor + product_name + product_version;
    modalias += board_vendor + board_name + board_version;
    modalias += chassis_vendor;
    if (chassis_type >= 0) {
        modalias += "ct" + std::to_string(chassis_type) + ":";
    }
    modalias += chassis_version;
    modalias += product_sku + product_family;
    return modalias;
}

}  // namespace aidl::android::hardware::sensors::mainline
