/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace aidl::android::hardware::camera::mainline {

// Reads a sysfs attribute with trailing whitespace removed.
std::optional<std::string> ReadSysfsString(const std::string& path);

// Reads a hexadecimal sysfs attribute such as a USB idVendor.
std::optional<uint32_t> ReadSysfsHex(const std::string& path);

// Canonical path of `path` with all symlinks resolved, empty on failure.
std::string CanonicalPath(const std::string& path);

// Walks up from the sysfs device directory `device_dir` to the first
// directory that has an attribute called `attribute` (inclusive). Returns
// that directory, or an empty string when /sys/devices is reached.
std::string FindSysfsAncestorWith(const std::string& device_dir, const std::string& attribute);

}  // namespace aidl::android::hardware::camera::mainline
