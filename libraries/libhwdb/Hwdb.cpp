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

#include <cctype>

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

namespace {

// systemd strips trailing whitespace with isspace(3).
void StripTrailingWhitespace(std::string* line) {
    while (!line->empty() && std::isspace(static_cast<unsigned char>(line->back()))) {
        line->pop_back();
    }
}

bool IsBlank(char c) {
    return c == ' ' || c == '\t';
}

}  // namespace

/*
 * Port of import_file() in systemd's src/shared/hwdb-util.c.
 *
 * A record is a group of match patterns (unindented lines) followed by
 * properties (lines starting with a single space), terminated by an empty
 * line. A '#' in the first column marks a comment line; anywhere else it
 * starts a trailing comment which is stripped from every line, including
 * property lines. Only ' ' marks a property, not '\t'.
 *
 * The quirks of this state machine are reproduced on purpose: hwdb files are
 * authored and validated against systemd, so parsing them any differently
 * would apply properties systemd would not (or the other way round).
 */
bool Hwdb::Parse(const std::string& content) {
    entries_.clear();

    enum class State { kNone, kMatch, kData };
    State state = State::kNone;
    Entry entry;
    size_t line_number = 0;

    auto finish_record = [&]() {
        if (!entry.match_patterns.empty() && !entry.properties.empty()) {
            entries_.push_back(std::move(entry));
        }
        entry = Entry{};
    };

    /*
     * Adds one "<blank><key>=<value>" property line to the record being
     * parsed. Port of insert_data(): the key is everything between the
     * leading blanks and the first '=', the value is the remainder of the
     * line taken verbatim (trailing whitespace has already been removed from
     * the whole line).
     */
    auto insert_property = [&](const std::string& line) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            LOG(WARNING) << "hwdb:" << line_number << ": key-value pair expected but got \"" << line
                         << "\", ignoring";
            return;
        }

        // Replace multiple leading blanks by a single one, then drop that one:
        // it is the marker that distinguishes properties from match patterns.
        size_t key_start = 0;
        while (key_start + 1 < equals && IsBlank(line[key_start]) && IsBlank(line[key_start + 1])) {
            key_start++;
        }
        key_start++;
        if (key_start >= equals) {
            LOG(WARNING) << "hwdb:" << line_number << ": empty key in \"" << line << "\", ignoring";
            return;
        }

        entry.properties[line.substr(key_start, equals - key_start)] = line.substr(equals + 1);
    };

    for (const auto& raw_line : ::android::base::Split(content, "\n")) {
        line_number++;

        if (::android::base::StartsWith(raw_line, "#")) {
            continue;
        }
        std::string line = raw_line.substr(0, raw_line.find('#'));
        StripTrailingWhitespace(&line);

        switch (state) {
            case State::kNone:
                if (line.empty()) {
                    break;
                }
                if (line[0] == ' ') {
                    LOG(WARNING) << "hwdb:" << line_number
                                 << ": match expected but got indented property \"" << line
                                 << "\", ignoring line";
                    break;
                }
                // Start of a record, first match pattern.
                state = State::kMatch;
                entry.match_patterns.push_back(line);
                break;

            case State::kMatch:
                if (line.empty()) {
                    LOG(WARNING) << "hwdb:" << line_number
                                 << ": property expected, ignoring record with no properties";
                    entry = Entry{};
                    state = State::kNone;
                    break;
                }
                if (line[0] != ' ') {
                    // Another match pattern for the same record.
                    entry.match_patterns.push_back(line);
                    break;
                }
                state = State::kData;
                insert_property(line);
                break;

            case State::kData:
                if (line.empty()) {
                    // End of the record.
                    finish_record();
                    state = State::kNone;
                    break;
                }
                if (line[0] != ' ') {
                    LOG(WARNING) << "hwdb:" << line_number
                                 << ": property or empty line expected, got \"" << line
                                 << "\", ignoring the rest of the record";
                    // Properties seen so far are kept, like systemd does.
                    finish_record();
                    state = State::kNone;
                    break;
                }
                insert_property(line);
                break;
        }
    }

    if (state == State::kMatch) {
        LOG(WARNING) << "hwdb: property expected, ignoring last record with no properties";
    } else if (state == State::kData) {
        finish_record();
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
