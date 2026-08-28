/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace libhwdb {

class Hwdb {
  public:
    static std::unique_ptr<Hwdb> FromFile(const std::string& path);
    static std::unique_ptr<Hwdb> FromContent(const std::string& content);

    std::map<std::string, std::string> GetProperties(const std::string& query) const;

    std::string GetProperty(const std::string& query, const std::string& key,
                            const std::string& default_value = "") const;

    std::map<std::string, std::string> GetProperties(
            const std::vector<std::string>& queries) const;

  private:
    struct Entry {
        std::vector<std::string> match_patterns;
        std::map<std::string, std::string> properties;
    };

    std::vector<Entry> entries_;

    bool Parse(const std::string& content);
    void MatchAndCollect(const std::string& query,
                         std::map<std::string, std::string>& result) const;
};

}  // namespace libhwdb
