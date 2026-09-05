/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#ifndef GRALLOC_GBM_MESA_ALLOCATOR_H_
#define GRALLOC_GBM_MESA_ALLOCATOR_H_

#include <aidl/android/hardware/graphics/allocator/AllocationResult.h>
#include <aidl/android/hardware/graphics/allocator/BnAllocator.h>
#include <android/hardware/graphics/mapper/4.0/IMapper.h>
#include <android/hardware/graphics/common/1.2/types.h>

#include "gralloc_gbm_mesa.h"

using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::graphics::common::PixelFormat;
using aidl::android::hardware::graphics::common::BufferUsage;

namespace aidl::android::hardware::graphics::allocator::impl {

class GbmMesaAllocator : public BnAllocator {
    public:
        GbmMesaAllocator() = default;
        ~GbmMesaAllocator();

        bool init();
        bool isInitialized();

        ndk::ScopedAStatus allocate(const std::vector<uint8_t>& descriptor, int32_t count,
                                    allocator::AllocationResult* outResult) override;

        ndk::ScopedAStatus allocate2(const BufferDescriptorInfo& descriptor, int32_t count,
                                     allocator::AllocationResult* outResult) override;

        ndk::ScopedAStatus isSupported(const BufferDescriptorInfo& descriptor,
                                       bool* outResult) override;

        ndk::ScopedAStatus getIMapperLibrarySuffix(std::string* outResult) override;

    protected:
        ndk::SpAIBinder createBinder() override;

    private:
        int _gbmDevFd = -1;

        ndk::ScopedAStatus gbmAllocateBuffer(const gralloc_buffer_desc& desc, int32_t* outStride, native_handle_t** outHandle);

        ndk::ScopedAStatus doAllocate(gralloc_buffer_desc& desc, int32_t count, allocator::AllocationResult* outResult);
};

} // namespace aidl::android::hardware::graphics::allocator::impl


#endif // GRALLOC_GBM_MESA_ALLOCATOR_H_