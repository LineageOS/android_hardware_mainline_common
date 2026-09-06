/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsSettings"

#include "libsensors_common/Settings.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parsebool.h>
#include <android-base/parsedouble.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#include "libsensors_common/Sysfs.h"

namespace aidl::android::hardware::sensors::mainline {

namespace {

std::optional<bool> ParseBoolValue(const std::string& value) {
    switch (::android::base::ParseBool(value)) {
        case ::android::base::ParseBoolResult::kTrue:
            return true;
        case ::android::base::ParseBoolResult::kFalse:
            return false;
        default:
            return std::nullopt;
    }
}

std::optional<int64_t> ParseIntValue(const std::string& value) {
    int64_t result = 0;
    if (!::android::base::ParseInt(value.c_str(), &result)) {
        return std::nullopt;
    }
    return result;
}

std::optional<double> ParseDoubleValue(const std::string& value) {
    double result = 0.0;
    if (!::android::base::ParseDouble(value.c_str(), &result)) {
        return std::nullopt;
    }
    return result;
}

}  // namespace

Settings& Settings::Get() {
    static Settings* instance = new Settings();
    return *instance;
}

Settings::Settings() {
    Reload();
}

void Settings::Reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    file_values_.clear();
    loaded_files_.clear();
    // Later directories override earlier ones, so the more specific /odm comes
    // last in kConfigDirs.
    for (const char* dir : kConfigDirs) {
        for (const auto& entry : sysfs::ListDirectory(dir)) {
            if (!::android::base::EndsWith(entry, ".conf")) {
                continue;
            }
            LoadFile(std::string(dir) + "/" + entry);
        }
    }
    LOG(INFO) << "Loaded " << loaded_files_.size() << " configuration file(s), "
              << file_values_.size() << " value(s)";
}

void Settings::LoadFile(const std::string& path) {
    std::string content;
    if (!::android::base::ReadFileToString(path, &content)) {
        LOG(WARNING) << "Cannot read configuration file " << path;
        return;
    }

    std::string section;
    size_t line_number = 0;
    for (const auto& raw_line : ::android::base::Split(content, "\n")) {
        line_number++;
        std::string line = ::android::base::Trim(raw_line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = ::android::base::Trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            LOG(WARNING) << path << ":" << line_number << ": ignoring malformed line '" << line
                         << "'";
            continue;
        }
        std::string key = ::android::base::Trim(line.substr(0, eq));
        std::string value = ::android::base::Trim(line.substr(eq + 1));
        if (key.empty()) {
            LOG(WARNING) << path << ":" << line_number << ": empty key";
            continue;
        }
        if (!section.empty()) {
            key = section + "." + key;
        }
        file_values_[key] = value;
        LOG(VERBOSE) << path << ": " << key << " = '" << value << "'";
    }
    loaded_files_.push_back(path);
    LOG(INFO) << "Loaded configuration file " << path;
}

std::optional<std::string> Settings::GetFromFiles(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = file_values_.find(key);
    if (it == file_values_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> Settings::GetString(const std::string& key) const {
    std::string property = ::android::base::GetProperty(std::string(kPropertyPrefix) + key, "");
    if (!property.empty()) {
        LOG(DEBUG) << "Setting " << key << " = '" << property << "' (property)";
        return property;
    }
    auto value = GetFromFiles(key);
    if (value.has_value()) {
        LOG(DEBUG) << "Setting " << key << " = '" << *value << "' (config file)";
    }
    return value;
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) const {
    return GetString(key).value_or(default_value);
}

std::optional<bool> Settings::GetBool(const std::string& key) const {
    auto value = GetString(key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    auto parsed = ParseBoolValue(*value);
    if (!parsed.has_value()) {
        LOG(WARNING) << "Setting " << key << " has invalid boolean value '" << *value << "'";
    }
    return parsed;
}

bool Settings::GetBool(const std::string& key, bool default_value) const {
    return GetBool(key).value_or(default_value);
}

std::optional<int64_t> Settings::GetInt(const std::string& key) const {
    auto value = GetString(key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    auto parsed = ParseIntValue(*value);
    if (!parsed.has_value()) {
        LOG(WARNING) << "Setting " << key << " has invalid integer value '" << *value << "'";
    }
    return parsed;
}

int64_t Settings::GetInt(const std::string& key, int64_t default_value) const {
    return GetInt(key).value_or(default_value);
}

std::optional<double> Settings::GetDouble(const std::string& key) const {
    auto value = GetString(key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    auto parsed = ParseDoubleValue(*value);
    if (!parsed.has_value()) {
        LOG(WARNING) << "Setting " << key << " has invalid numeric value '" << *value << "'";
    }
    return parsed;
}

double Settings::GetDouble(const std::string& key, double default_value) const {
    return GetDouble(key).value_or(default_value);
}

std::optional<std::string> Settings::GetFirstString(const std::vector<std::string>& keys) const {
    for (const auto& key : keys) {
        auto value = GetString(key);
        if (value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<bool> Settings::GetFirstBool(const std::vector<std::string>& keys) const {
    for (const auto& key : keys) {
        auto value = GetBool(key);
        if (value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<int64_t> Settings::GetFirstInt(const std::vector<std::string>& keys) const {
    for (const auto& key : keys) {
        auto value = GetInt(key);
        if (value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<double> Settings::GetFirstDouble(const std::vector<std::string>& keys) const {
    for (const auto& key : keys) {
        auto value = GetDouble(key);
        if (value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::string Settings::SanitizeKeyComponent(const std::string& component) {
    std::string result = component;
    for (char& c : result) {
        bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '_';
        if (!valid) {
            c = '_';
        }
    }
    return result;
}

std::vector<std::string> Settings::GetLoadedFiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_files_;
}

}  // namespace aidl::android::hardware::sensors::mainline
