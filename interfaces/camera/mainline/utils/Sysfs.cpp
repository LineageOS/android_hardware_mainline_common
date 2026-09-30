/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "utils/Sysfs.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

namespace aidl::android::hardware::camera::mainline {

std::optional<std::string> ReadSysfsString(const std::string& path) {
    std::string content;
    if (!::android::base::ReadFileToString(path, &content)) return std::nullopt;
    // Some drivers terminate the value with a NUL, before or after the
    // newline. Text attributes never contain one otherwise, so cut there;
    // Trim() then removes the newline and other whitespace.
    if (const size_t nul = content.find('\0'); nul != std::string::npos) content.resize(nul);
    return ::android::base::Trim(content);
}

std::optional<uint32_t> ReadSysfsHex(const std::string& path) {
    const auto content = ReadSysfsString(path);
    if (!content.has_value()) return std::nullopt;
    std::string value = *content;
    if (!::android::base::StartsWith(value, "0x")) value = "0x" + value;
    uint32_t result;
    if (!::android::base::ParseUint(value, &result)) return std::nullopt;
    return result;
}

std::string CanonicalPath(const std::string& path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) == nullptr) return {};
    return resolved;
}

std::string FindSysfsAncestorWith(const std::string& device_dir, const std::string& attribute) {
    std::string dir = device_dir;
    while (::android::base::StartsWith(dir, "/sys/devices/")) {
        if (access((dir + "/" + attribute).c_str(), F_OK) == 0) return dir;
        const size_t slash = dir.rfind('/');
        if (slash == std::string::npos) break;
        dir.resize(slash);
    }
    return {};
}

}  // namespace aidl::android::hardware::camera::mainline
