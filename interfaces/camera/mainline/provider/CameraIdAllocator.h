/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <set>
#include <string>

namespace aidl::android::hardware::camera::mainline {

// Hands out numerical camera IDs. Internal cameras get IDs below
// `external_offset`, external ones from `external_offset` on, both the
// smallest one that is free.
//
// A camera that comes back (e.g. a USB camera replugged into the same port)
// gets the ID it had before, as long as nothing else took it meanwhile.
class CameraIdAllocator {
  public:
    explicit CameraIdAllocator(int external_offset) : external_offset_(external_offset) {}

    int Allocate(const std::string& key, bool internal);
    void Release(const std::string& key);

  private:
    bool InRange(int id, bool internal) const {
        return internal ? id < external_offset_ : id >= external_offset_;
    }

    const int external_offset_;
    // Every ID ever handed out, including released ones.
    std::map<std::string, int> assigned_;
    std::set<int> in_use_;
};

}  // namespace aidl::android::hardware::camera::mainline
