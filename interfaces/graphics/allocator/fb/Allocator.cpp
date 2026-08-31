/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-allocator"

#include "Allocator.h"

#include <aidl/android/hardware/graphics/allocator/AllocationError.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android/binder_ibinder_platform.h>
#include <android/hardware/graphics/mapper/4.0/IMapper.h>
#include <gralloctypes/Gralloc4.h>
#include <log/log.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>

#include "Buffer.h"

namespace aidl::android::hardware::graphics::allocator::impl {
namespace {

ndk::ScopedAStatus Error(AllocationError error) {
    return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(error));
}

}  // namespace

ndk::ScopedAStatus Allocator::allocate(const std::vector<uint8_t>& encoded, int32_t count,
                                       AllocationResult* result) {
    ::android::hardware::graphics::mapper::V4_0::IMapper::BufferDescriptorInfo old;
    if (::android::gralloc4::decodeBufferDescriptorInfo(encoded, &old) != 0) {
        return Error(AllocationError::BAD_DESCRIPTOR);
    }
    BufferDescriptorInfo descriptor;
    const size_t name_size = std::min(old.name.size(), descriptor.name.size() - 1);
    memcpy(descriptor.name.data(), old.name.c_str(), name_size);
    descriptor.width = old.width;
    descriptor.height = old.height;
    descriptor.layerCount = old.layerCount;
    descriptor.format = static_cast<common::PixelFormat>(old.format);
    descriptor.usage = static_cast<common::BufferUsage>(old.usage);
    descriptor.reservedSize = old.reservedSize;
    return Allocate(descriptor, count, result);
}

ndk::ScopedAStatus Allocator::allocate2(const BufferDescriptorInfo& descriptor, int32_t count,
                                        AllocationResult* result) {
    return Allocate(descriptor, count, result);
}

ndk::ScopedAStatus Allocator::Allocate(const BufferDescriptorInfo& descriptor, int32_t count,
                                       AllocationResult* result) {
    if (count <= 0 || count > 4096) return Error(AllocationError::BAD_DESCRIPTOR);
    if (descriptor.width <= 0 || descriptor.height <= 0 || descriptor.layerCount != 1 ||
        descriptor.reservedSize < 0 ||
        static_cast<uint64_t>(descriptor.reservedSize) > fb::kMaxReservedSize) {
        return Error(AllocationError::BAD_DESCRIPTOR);
    }
    fb::BufferLayout layout;
    std::string reason;
    if (!fb::BuildLayout(descriptor, &layout, &reason)) {
        ALOGW("Rejected %dx%d %s allocation: %s", descriptor.width, descriptor.height,
              fb::FormatName(descriptor.format).c_str(), reason.c_str());
        return Error(AllocationError::UNSUPPORTED);
    }
    result->buffers.clear();
    result->buffers.reserve(count);
    result->stride = layout.pixel_stride;
    for (int32_t i = 0; i < count; ++i) {
        uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        id ^= static_cast<uint64_t>(getpid()) << 32;
        native_handle_t* handle = fb::AllocateHandle(layout, id, &reason);
        if (handle == nullptr) {
            ALOGE("Allocation failed: %s", reason.c_str());
            result->buffers.clear();
            return Error(AllocationError::NO_RESOURCES);
        }
        auto aidl_handle = ::android::dupToAidl(handle);
        const bool duplicated = std::all_of(aidl_handle.fds.begin(), aidl_handle.fds.end(),
                                            [](const auto& fd) { return fd.get() >= 0; });
        native_handle_close(handle);
        native_handle_delete(handle);
        if (!duplicated) {
            result->buffers.clear();
            return Error(AllocationError::NO_RESOURCES);
        }
        result->buffers.push_back(std::move(aidl_handle));
    }
    ALOGV("Allocated count=%d id-next=%" PRIu64 " %ux%u %s stride=%u size=%" PRIu64, count,
          next_id_.load(), layout.width, layout.height, fb::FormatName(layout.format).c_str(),
          layout.pixel_stride, layout.allocation_size);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Allocator::isSupported(const BufferDescriptorInfo& descriptor, bool* result) {
    fb::BufferLayout layout;
    *result = fb::BuildLayout(descriptor, &layout, nullptr);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Allocator::getIMapperLibrarySuffix(std::string* suffix) {
    *suffix = "fb";
    return ndk::ScopedAStatus::ok();
}

ndk::SpAIBinder Allocator::createBinder() {
    auto binder = BnAllocator::createBinder();
    AIBinder_setInheritRt(binder.get(), true);
    return binder;
}

}  // namespace aidl::android::hardware::graphics::allocator::impl
