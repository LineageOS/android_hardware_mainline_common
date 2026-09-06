/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

/*
 * Unified access to HAL configuration.
 *
 * Every setting is identified by a dotted key such as "iio.bmi160.mount_matrix".
 * A lookup tries, in order:
 *   1. the Android property "vendor.sensors.<key>",
 *   2. the configuration files "/odm/etc/sensors/<name>.conf" (highest priority)
 *      and "/vendor/etc/sensors/<name>.conf".
 *
 * Configuration file format (INI-like):
 *   # comment
 *   backends = iio,input
 *   [iio.bmi160]            ; section: prefix for the keys below
 *   mount_matrix = 0, -1, 0; -1, 0, 0; 0, 0, 1
 *   power = 0.15
 *
 * Device names used inside keys must be sanitized with SanitizeKeyComponent().
 */
class Settings {
  public:
    static constexpr const char* kPropertyPrefix = "vendor.sensors.";
    static constexpr const char* kConfigDirs[] = {"/vendor/etc/sensors", "/odm/etc/sensors"};

    // Returns the process wide instance, loading the configuration files on
    // first use.
    static Settings& Get();

    std::optional<std::string> GetString(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& default_value) const;

    std::optional<bool> GetBool(const std::string& key) const;
    bool GetBool(const std::string& key, bool default_value) const;

    std::optional<int64_t> GetInt(const std::string& key) const;
    int64_t GetInt(const std::string& key, int64_t default_value) const;

    std::optional<double> GetDouble(const std::string& key) const;
    double GetDouble(const std::string& key, double default_value) const;

    // Returns the value of the first key that is set. Used to implement
    // "specific key first, generic key second" lookups.
    std::optional<std::string> GetFirstString(const std::vector<std::string>& keys) const;
    std::optional<bool> GetFirstBool(const std::vector<std::string>& keys) const;
    std::optional<int64_t> GetFirstInt(const std::vector<std::string>& keys) const;
    std::optional<double> GetFirstDouble(const std::vector<std::string>& keys) const;

    // Replaces every character that is not [A-Za-z0-9_] by '_' so that the
    // result can be used both in property names and in configuration keys.
    static std::string SanitizeKeyComponent(const std::string& component);

    // Re-reads the configuration files.
    void Reload();

    // Files that were successfully parsed, for logging/debugging.
    std::vector<std::string> GetLoadedFiles() const;

  private:
    Settings();

    void LoadFile(const std::string& path);
    std::optional<std::string> GetFromFiles(const std::string& key) const;

    mutable std::mutex mutex_;
    std::map<std::string, std::string> file_values_;
    std::vector<std::string> loaded_files_;
};

}  // namespace aidl::android::hardware::sensors::mainline
