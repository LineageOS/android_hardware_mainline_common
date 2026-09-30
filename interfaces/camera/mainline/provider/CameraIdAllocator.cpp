/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "provider/CameraIdAllocator.h"

namespace aidl::android::hardware::camera::mainline {

int CameraIdAllocator::Allocate(const std::string& key, bool internal) {
    if (auto it = assigned_.find(key); it != assigned_.end()) {
        const int previous = it->second;
        if (InRange(previous, internal) && in_use_.count(previous) == 0) {
            in_use_.insert(previous);
            return previous;
        }
    }

    int id = internal ? 0 : external_offset_;
    while (in_use_.count(id) != 0) ++id;
    // Internal IDs running into the external range can only happen with more
    // than `external_offset_` internal cameras; they then simply continue
    // there, which is still unique.
    in_use_.insert(id);
    assigned_[key] = id;
    return id;
}

void CameraIdAllocator::Release(const std::string& key) {
    if (auto it = assigned_.find(key); it != assigned_.end()) in_use_.erase(it->second);
}

}  // namespace aidl::android::hardware::camera::mainline
