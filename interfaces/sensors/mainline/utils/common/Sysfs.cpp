/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsSysfs"

#include "libsensors_common/Sysfs.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace aidl::android::hardware::sensors::mainline::sysfs {

bool Exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool IsDirectory(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

std::vector<std::string> ListDirectory(const std::string& path) {
    std::vector<std::string> entries;
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) {
        return entries;
    }
    for (const auto& entry : it) {
        entries.push_back(entry.path().filename().string());
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

std::optional<std::string> ReadString(const std::string& path) {
    std::string content;
    if (!::android::base::ReadFileToString(path, &content)) {
        return std::nullopt;
    }
    // Some drivers emit a trailing NUL or newline; remove them all before
    // trimming regular whitespace.
    while (!content.empty() && (content.back() == '\0' || content.back() == '\n')) {
        content.pop_back();
    }
    return ::android::base::Trim(content);
}

std::string ReadString(const std::string& path, const std::string& default_value) {
    return ReadString(path).value_or(default_value);
}

std::optional<int64_t> ReadInt(const std::string& path) {
    auto content = ReadString(path);
    if (!content.has_value()) {
        return std::nullopt;
    }
    int64_t value = 0;
    if (!::android::base::ParseInt(content->c_str(), &value)) {
        LOG(DEBUG) << "Not an integer in " << path << ": '" << *content << "'";
        return std::nullopt;
    }
    return value;
}

int64_t ReadInt(const std::string& path, int64_t default_value) {
    return ReadInt(path).value_or(default_value);
}

std::optional<double> ReadDouble(const std::string& path) {
    auto content = ReadString(path);
    if (!content.has_value()) {
        return std::nullopt;
    }
    double value = 0.0;
    if (!::android::base::ParseDouble(content->c_str(), &value)) {
        LOG(DEBUG) << "Not a number in " << path << ": '" << *content << "'";
        return std::nullopt;
    }
    return value;
}

double ReadDouble(const std::string& path, double default_value) {
    return ReadDouble(path).value_or(default_value);
}

bool WriteString(const std::string& path, const std::string& value) {
    if (!::android::base::WriteStringToFile(value, path)) {
        LOG(DEBUG) << "Failed to write '" << value << "' to " << path << ": " << strerror(errno);
        return false;
    }
    return true;
}

bool WriteInt(const std::string& path, int64_t value) {
    return WriteString(path, std::to_string(value));
}

bool WriteDouble(const std::string& path, double value) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.6f", value);
    return WriteString(path, buffer);
}

std::vector<double> ParseDoubleList(const std::string& content) {
    std::vector<double> values;
    std::string cleaned = content;
    std::replace(cleaned.begin(), cleaned.end(), '[', ' ');
    std::replace(cleaned.begin(), cleaned.end(), ']', ' ');
    std::vector<std::string> tokens = ::android::base::Tokenize(cleaned, " \t");
    for (const auto& token : tokens) {
        double value = 0.0;
        if (::android::base::ParseDouble(token.c_str(), &value)) {
            values.push_back(value);
        }
    }
    // "[min step max]" ranges: keep the bounds only.
    if (content.find('[') != std::string::npos && values.size() == 3) {
        values = {values[0], values[2]};
    }
    std::sort(values.begin(), values.end());
    return values;
}

std::string RealPath(const std::string& path) {
    std::string resolved;
    if (::android::base::Realpath(path, &resolved)) {
        return resolved;
    }
    return path;
}

}  // namespace aidl::android::hardware::sensors::mainline::sysfs
