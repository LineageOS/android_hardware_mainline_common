/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/graphics/allocator/AllocationResult.h>
#include <aidl/android/hardware/graphics/allocator/BnAllocator.h>

#include <atomic>

namespace aidl::android::hardware::graphics::allocator::impl {

class Allocator : public BnAllocator {
  public:
    ndk::ScopedAStatus allocate(const std::vector<uint8_t>& descriptor, int32_t count,
                                AllocationResult* result) override;
    ndk::ScopedAStatus allocate2(const BufferDescriptorInfo& descriptor, int32_t count,
                                 AllocationResult* result) override;
    ndk::ScopedAStatus isSupported(const BufferDescriptorInfo& descriptor, bool* result) override;
    ndk::ScopedAStatus getIMapperLibrarySuffix(std::string* suffix) override;
    ndk::ScopedAStatus isMultiViewSupported(const std::vector<BufferDescriptorInfo>& descriptors,
                                            int32_t base_view_index, bool* result) override;
    ndk::ScopedAStatus allocateMultiView(const std::vector<BufferDescriptorInfo>& descriptors,
                                         int32_t base_view_index,
                                         AllocationResult* result) override;

  protected:
    ndk::SpAIBinder createBinder() override;

  private:
    ndk::ScopedAStatus Allocate(const BufferDescriptorInfo& descriptor, int32_t count,
                                AllocationResult* result);
    std::atomic<uint64_t> next_id_{1};
};

}  // namespace aidl::android::hardware::graphics::allocator::impl
