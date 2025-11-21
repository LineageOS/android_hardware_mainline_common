/*
 * SPDX-FileCopyrightText: 2022 The Android Open Source Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/boot/BnBootControl.h>

#include <GrubBootControl.h>

namespace aidl::android::hardware::boot {

class BootControl final : public BnBootControl {
  public:
    BootControl();
    ::ndk::ScopedAStatus getActiveBootSlot(int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus getCurrentSlot(int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus getNumberSlots(int32_t* _aidl_return) override;
    ::ndk::ScopedAStatus getSnapshotMergeStatus(
            ::aidl::android::hardware::boot::MergeStatus* _aidl_return) override;
    ::ndk::ScopedAStatus getSuffix(int32_t in_slot, std::string* _aidl_return) override;
    ::ndk::ScopedAStatus isSlotBootable(int32_t in_slot, bool* _aidl_return) override;
    ::ndk::ScopedAStatus isSlotMarkedSuccessful(int32_t in_slot, bool* _aidl_return) override;
    ::ndk::ScopedAStatus markBootSuccessful() override;
    ::ndk::ScopedAStatus setActiveBootSlot(int32_t in_slot) override;
    ::ndk::ScopedAStatus setSlotAsUnbootable(int32_t in_slot) override;
    ::ndk::ScopedAStatus setSnapshotMergeStatus(
            ::aidl::android::hardware::boot::MergeStatus in_status) override;

  private:
    ::libgrub_boot_control::GrubBootControl* mBackend;
};

}  // namespace aidl::android::hardware::boot
