/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/camera/common/Status.h>
#include <android/binder_auto_utils.h>

namespace aidl::android::hardware::camera::mainline {

// Camera HAL errors travel as service specific errors carrying a
// camera::common::Status.
inline ::ndk::ScopedAStatus ToBinderStatus(common::Status status) {
    if (status == common::Status::OK) return ::ndk::ScopedAStatus::ok();
    return ::ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(status));
}

}  // namespace aidl::android::hardware::camera::mainline
