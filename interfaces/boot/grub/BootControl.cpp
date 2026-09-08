/*
 * SPDX-FileCopyrightText: 2022 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "BootControl.h"
#include <cstdint>

#include <android-base/logging.h>

using ndk::ScopedAStatus;

namespace aidl::android::hardware::boot {

namespace {

std::string ConvertMergeStatusToGrubString(MergeStatus merge_status) {
    switch (merge_status) {
        case MergeStatus::NONE:
            return "none";
        case MergeStatus::UNKNOWN:
            return "unknown";
        case MergeStatus::SNAPSHOTTED:
            return "snapshotted";
        case MergeStatus::MERGING:
            return "merging";
        case MergeStatus::CANCELLED:
            return "cancelled";
        default:
            return "";
    }
}

MergeStatus ConvertGrubStringToMergeStatus(std::string str) {
    // Nothing has ever been recorded, which simply means that there is no
    // snapshot around rather than that we failed to figure it out
    if (str.empty()) return MergeStatus::NONE;
    if (str == "none") return MergeStatus::NONE;
    if (str == "unknown") return MergeStatus::UNKNOWN;
    if (str == "snapshotted") return MergeStatus::SNAPSHOTTED;
    if (str == "merging") return MergeStatus::MERGING;
    if (str == "cancelled") return MergeStatus::CANCELLED;
    return MergeStatus::UNKNOWN;
}

}  // namespace

BootControl::BootControl() {
#if defined(__ANDROID_RECOVERY__)
    mBackend = std::make_unique<libgrub_boot_control::GrubBootControl>(
            "/mnt/vendor/_persist/grubenv_abootctrl");
    // Booting into recovery is not an attempt to boot the slot itself
    mBackend->DecreaseRetryCountForCurrentSlot();
#else
    mBackend = std::make_unique<libgrub_boot_control::GrubBootControl>();
#endif
}

ScopedAStatus BootControl::getActiveBootSlot(int32_t* _aidl_return) {
    int32_t val = mBackend->getActiveBootSlot();
    if (val == COMMAND_FAILED) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                  "Operation failed");
    }
    *_aidl_return = val;
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::getCurrentSlot(int32_t* _aidl_return) {
    int32_t val = mBackend->getCurrentSlot();
    if (val == COMMAND_FAILED) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                  "Operation failed");
    }
    *_aidl_return = val;
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::getNumberSlots(int32_t* _aidl_return) {
    *_aidl_return = mBackend->getNumberSlots();
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::getSnapshotMergeStatus(MergeStatus* _aidl_return) {
    *_aidl_return = ConvertGrubStringToMergeStatus(mBackend->getSnapshotMergeStatus());
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::getSuffix(int32_t in_slot, std::string* _aidl_return) {
    *_aidl_return = mBackend->getSuffix(in_slot);
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::isSlotBootable(int32_t in_slot, bool* _aidl_return) {
    int32_t val = mBackend->isSlotBootable(in_slot);
    if (val == INVALID_SLOT) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                INVALID_SLOT, (std::string("Invalid slot ") + std::to_string(in_slot)).c_str());
    }
    *_aidl_return = val;
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::isSlotMarkedSuccessful(int32_t in_slot, bool* _aidl_return) {
    int32_t val = mBackend->isSlotMarkedSuccessful(in_slot);
    if (val == INVALID_SLOT) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                INVALID_SLOT, (std::string("Invalid slot ") + std::to_string(in_slot)).c_str());
    }
    *_aidl_return = val;
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::markBootSuccessful() {
    int32_t ret = mBackend->markBootSuccessful();
    if (ret == COMMAND_FAILED) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                  "Operation failed");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::setActiveBootSlot(int32_t in_slot) {
    int32_t ret = mBackend->setActiveBootSlot(in_slot);
    switch (ret) {
        case COMMAND_FAILED:
            return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                      "Operation failed");
        case INVALID_SLOT:
            return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                    INVALID_SLOT, (std::string("Invalid slot ") + std::to_string(in_slot)).c_str());
        default:
            break;
    }
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::setSlotAsUnbootable(int32_t in_slot) {
    int32_t ret = mBackend->setSlotAsUnbootable(in_slot);
    switch (ret) {
        case COMMAND_FAILED:
            return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                      "Operation failed");
        case INVALID_SLOT:
            return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                    INVALID_SLOT, (std::string("Invalid slot ") + std::to_string(in_slot)).c_str());
        default:
            break;
    }
    return ScopedAStatus::ok();
}

ScopedAStatus BootControl::setSnapshotMergeStatus(MergeStatus in_status) {
    const std::string status_str = ConvertMergeStatusToGrubString(in_status);
    if (status_str.empty()) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                  "Unknown merge status");
    }

    int32_t ret = mBackend->setSnapshotMergeStatus(status_str);
    if (ret == COMMAND_FAILED) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                  "Operation failed");
    }
    return ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::boot
