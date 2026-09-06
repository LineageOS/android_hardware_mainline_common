/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aidl::android::hardware::sensors::mainline::sysfs {

// Returns true if `path` exists (any file type).
bool Exists(const std::string& path);

// Returns true if `path` exists and is a directory.
bool IsDirectory(const std::string& path);

// Lists the entry names (not full paths) of a directory, sorted. Returns an
// empty vector if the directory cannot be read.
std::vector<std::string> ListDirectory(const std::string& path);

// Reads a sysfs attribute as a string. Trailing '\0' and '\n' characters as
// well as surrounding whitespace are removed.
std::optional<std::string> ReadString(const std::string& path);
std::string ReadString(const std::string& path, const std::string& default_value);

// Reads a sysfs attribute and parses it as a decimal integer.
std::optional<int64_t> ReadInt(const std::string& path);
int64_t ReadInt(const std::string& path, int64_t default_value);

// Reads a sysfs attribute and parses it as a floating point value.
std::optional<double> ReadDouble(const std::string& path);
double ReadDouble(const std::string& path, double default_value);

// Writes a value to a sysfs attribute. Failures are logged at debug level; the
// caller decides how loud a failure is.
bool WriteString(const std::string& path, const std::string& value);
bool WriteInt(const std::string& path, int64_t value);
bool WriteDouble(const std::string& path, double value);

// Parses a whitespace separated list of floating point values, as found in
// "*_available" attributes. A "[min step max]" range is expanded to its bounds
// only (two values) since the step may be fractional.
std::vector<double> ParseDoubleList(const std::string& content);

// Resolves symlinks in `path`. Returns `path` unchanged on failure.
std::string RealPath(const std::string& path);

}  // namespace aidl::android::hardware::sensors::mainline::sysfs
