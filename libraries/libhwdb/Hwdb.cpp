/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "libhwdb"

#include <libhwdb/Hwdb.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <fnmatch.h>

namespace libhwdb {

std::unique_ptr<Hwdb> Hwdb::FromFile(const std::string& path) {
    std::string content;
    if (!::android::base::ReadFileToString(path, &content)) {
        LOG(WARNING) << "Failed to read hwdb file: " << path;
        return nullptr;
    }

    auto hwdb = std::unique_ptr<Hwdb>(new Hwdb());
    if (!hwdb->Parse(content)) {
        LOG(WARNING) << "Failed to parse hwdb file: " << path;
        return nullptr;
    }

    LOG(INFO) << "Parsed hwdb file: " << path << " (" << hwdb->entries_.size() << " entries)";
    return hwdb;
}

std::unique_ptr<Hwdb> Hwdb::FromContent(const std::string& content) {
    auto hwdb = std::unique_ptr<Hwdb>(new Hwdb());
    if (!hwdb->Parse(content)) {
        return nullptr;
    }
    return hwdb;
}

bool Hwdb::Parse(const std::string& content) {
    entries_.clear();

    Entry current_entry;
    bool collecting_properties = false;

    auto lines = ::android::base::Split(content, "\n");
    for (auto& raw_line : lines) {
        std::string line = ::android::base::Trim(raw_line);

        if (line.empty()) {
            continue;
        }

        if (line[0] == '#') {
            continue;
        }

        if (::android::base::StartsWith(raw_line, " ") ||
            ::android::base::StartsWith(raw_line, "\t")) {
            size_t eq_pos = line.find('=');
            if (eq_pos == std::string::npos) {
                continue;
            }

            std::string key = ::android::base::Trim(line.substr(0, eq_pos));
            std::string value = ::android::base::Trim(line.substr(eq_pos + 1));

            if (key.empty()) {
                continue;
            }

            current_entry.properties[key] = value;
            collecting_properties = true;
        } else {
            if (collecting_properties) {
                if (!current_entry.match_patterns.empty() &&
                    !current_entry.properties.empty()) {
                    entries_.push_back(std::move(current_entry));
                }
                current_entry = Entry{};
                collecting_properties = false;
            }

            size_t comment_pos = line.find('#');
            if (comment_pos != std::string::npos) {
                line = ::android::base::Trim(line.substr(0, comment_pos));
            }

            if (!line.empty()) {
                current_entry.match_patterns.push_back(line);
            }
        }
    }

    if (!current_entry.match_patterns.empty() && !current_entry.properties.empty()) {
        entries_.push_back(std::move(current_entry));
    }

    return true;
}

void Hwdb::MatchAndCollect(const std::string& query,
                            std::map<std::string, std::string>& result) const {
    for (const auto& entry : entries_) {
        bool matched = false;
        for (const auto& pattern : entry.match_patterns) {
            if (fnmatch(pattern.c_str(), query.c_str(), 0) == 0) {
                matched = true;
                break;
            }
        }

        if (matched) {
            for (const auto& [key, value] : entry.properties) {
                result[key] = value;
            }
        }
    }
}

std::map<std::string, std::string> Hwdb::GetProperties(const std::string& query) const {
    std::map<std::string, std::string> result;
    MatchAndCollect(query, result);
    return result;
}

std::string Hwdb::GetProperty(const std::string& query, const std::string& key,
                               const std::string& default_value) const {
    auto props = GetProperties(query);
    auto it = props.find(key);
    if (it != props.end()) {
        return it->second;
    }
    return default_value;
}

std::map<std::string, std::string> Hwdb::GetProperties(
        const std::vector<std::string>& queries) const {
    std::map<std::string, std::string> result;
    for (const auto& query : queries) {
        MatchAndCollect(query, result);
    }
    return result;
}

}  // namespace libhwdb
