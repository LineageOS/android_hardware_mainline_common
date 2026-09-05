/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#include "Allocator.h"

#include <aidl/android/hardware/graphics/allocator/AllocationError.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/logging.h>
#include <android/binder_ibinder_platform.h>
#include <gralloctypes/Gralloc4.h>

#include "log.h"

using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::graphics::common::ExtendableType;
using BufferDescriptorInfoV4 =
        android::hardware::graphics::mapper::V4_0::IMapper::BufferDescriptorInfo;

static const std::string STANDARD_METADATA_DATASPACE = "android.hardware.graphics.common.Dataspace";

namespace aidl::android::hardware::graphics::allocator::impl {

inline ndk::ScopedAStatus ToBinderStatus(AllocationError error) {
    return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(error));
}

ndk::ScopedAStatus convertToGBMDesc(const BufferDescriptorInfo& info, gralloc_buffer_desc* outResult) {
    if (info.width == 0 || info.height == 0) {
        log_e("Invalid buffer descriptor: width or height is zero");
        return ToBinderStatus(AllocationError::BAD_DESCRIPTOR);
    }

    outResult->width = static_cast<uint32_t>(info.width);
    outResult->height = static_cast<uint32_t>(info.height);

    if (info.layerCount > 1) {
        log_e("Failed to convert descriptor. Unsupported layerCount: %d", info.layerCount);
        return ToBinderStatus(AllocationError::UNSUPPORTED);
    }

    outResult->android_format = static_cast<uint32_t>(info.format);
    outResult->android_usage = static_cast<uint32_t>(info.usage);
    outResult->android_reserved_size = static_cast<uint32_t>(info.reservedSize);
    outResult->layer_count = static_cast<uint32_t>(info.layerCount);
    outResult->gbm_format = -1; // GBM FourCC format should be convert in gralloc_gm
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus GbmMesaAllocator::gbmAllocateBuffer(const gralloc_buffer_desc& desc, int32_t* outStride,
                                             native_handle_t** outHandle) {
    if (!isInitialized()) {
        log_e("gbmAllocateBuffer failed. Allocator is uninitialized.\n");
        return ToBinderStatus(AllocationError::NO_RESOURCES);
    }

    if (!gralloc_is_desc_support(&desc)) {
        const std::string pixelFormatString = ::android::hardware::graphics::common::V1_2::toString(
            static_cast<::android::hardware::graphics::common::V1_2::PixelFormat>(desc.android_format));
        const std::string usageString = ::android::hardware::graphics::common::V1_2::toString<::android::hardware::graphics::common::V1_2::BufferUsage>(
            static_cast<uint64_t>(desc.android_usage));
        log_e("Failed to allocate. Unsupported combination: pixel format:%s, usage:%s\n",
              pixelFormatString.c_str(), usageString.c_str());
        return ToBinderStatus(AllocationError::UNSUPPORTED);
    }

    native_handle_t* handle;
    int32_t stride = 0;
    int ret = gralloc_allocate(&desc, &stride, &handle);
    if (ret) {
        log_e("Failed to allocate. Error code: %d\n", ret);
        return ToBinderStatus(AllocationError::NO_RESOURCES);
    }

    int32_t pixelStride = gralloc_gm_android_caculate_pixel_stride(desc.android_format, stride);
    *outStride = static_cast<int32_t>(pixelStride);
    *outHandle = handle;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus GbmMesaAllocator::doAllocate(gralloc_buffer_desc& desc, int32_t count, 
    allocator::AllocationResult* outResult) {
    if (!isInitialized()) {
        log_e("doAllocate failed. Allocator is uninitialized.\n");
        return ToBinderStatus(AllocationError::NO_RESOURCES);
    }

    std::vector<native_handle_t*> handles;
    handles.resize(count, nullptr);
    
    for (int32_t i = 0; i < count; i++) {
        ndk::ScopedAStatus status = gbmAllocateBuffer(desc, &outResult->stride, &handles[i]);

        if (!status.isOk()) {
            for (int32_t j = 0; j < i; j++) {
                // Release all buffer and handle
                if (!handles[j])
                    continue;
                gralloc_gm_buffer_free(handles[j]);
                native_handle_close(handles[j]);
                native_handle_delete(handles[j]);
            }
            return status;
        }
    }

    outResult->buffers.resize(count);
    for (int32_t i = 0; i < count; i++) {
        if (!handles[i])
            continue;
        auto handle = handles[i];
        outResult->buffers[i] = ::android::dupToAidl(handle);
        // Release buffer and handle
        gralloc_gm_buffer_free(handle);
        native_handle_close(handle);
        native_handle_delete(handle);
    }

    return ndk::ScopedAStatus::ok();
}

GbmMesaAllocator::~GbmMesaAllocator() {
    if (_gbmDevFd > 0) {
        close(_gbmDevFd);
        _gbmDevFd = -1;
    }
}

bool GbmMesaAllocator::init() {
    _gbmDevFd = gralloc_gbm_device_init();
    return (_gbmDevFd > 0);
}

bool GbmMesaAllocator::isInitialized() {
    return (_gbmDevFd > 0);
}

ndk::ScopedAStatus GbmMesaAllocator::allocate(const std::vector<uint8_t>& encodedDescriptor, int32_t count,
                                       allocator::AllocationResult* outResult) {
    if (!isInitialized()) {
        log_e("Failed to allocate. Allocator is uninitialized.\n");
        return ToBinderStatus(AllocationError::NO_RESOURCES);
    }

    BufferDescriptorInfoV4 mapperV4Descriptor;

    int ret = ::android::gralloc4::decodeBufferDescriptorInfo(encodedDescriptor, &mapperV4Descriptor);
    if (ret) {
        log_e("Failed to allocate. Failed to decode buffer descriptor: %d.\n", ret);
        return ToBinderStatus(AllocationError::BAD_DESCRIPTOR);
    }

    struct gralloc_buffer_desc gbmDesc = {};
    const BufferDescriptorInfo info = {
        .name = {"auto_generated"},
        .width = static_cast<int32_t>(mapperV4Descriptor.width),
        .height = static_cast<int32_t>(mapperV4Descriptor.height),
        .layerCount = static_cast<int32_t>(mapperV4Descriptor.layerCount),
        .format = (PixelFormat) mapperV4Descriptor.format,
        .usage = (BufferUsage) mapperV4Descriptor.usage,
        .reservedSize = static_cast<int64_t>(mapperV4Descriptor.reservedSize),
    }; 

    ndk::ScopedAStatus status = convertToGBMDesc(info, &gbmDesc);
    if (!status.isOk()) {
        log_e("Failed to convert the request buffer desc to gbm desc.\n");
        return ToBinderStatus(AllocationError::UNSUPPORTED);
    }

    return doAllocate(gbmDesc, count, outResult);
}

ndk::ScopedAStatus GbmMesaAllocator::allocate2(const BufferDescriptorInfo& descriptor, int32_t count,
                            allocator::AllocationResult* outResult) {
    if (!isInitialized()) {
        log_e("Failed to allocate. Allocator is uninitialized.\n");
        return ToBinderStatus(AllocationError::NO_RESOURCES);
    }

    struct gralloc_buffer_desc gbmDesc = {};

    ndk::ScopedAStatus status = convertToGBMDesc(descriptor, &gbmDesc);
    if (!status.isOk()) {
        log_e("Failed to convert the request buffer desc to gbm desc.\n");
        return status;
    }

    return doAllocate(gbmDesc, count, outResult);
}

ndk::ScopedAStatus GbmMesaAllocator::isSupported(const BufferDescriptorInfo& descriptor,
                            bool* outResult) {
    if (!isInitialized()) {
        log_e("Failed to allocate. Allocator is uninitialized.\n");
        return ToBinderStatus(AllocationError::NO_RESOURCES);
    }

    for (const auto& option : descriptor.additionalOptions) {
        if (option.name != STANDARD_METADATA_DATASPACE) {
            *outResult = false;
            return ndk::ScopedAStatus::ok();
        }
    }

    // TODO: Call the gralloc_is_desc_support()
    *outResult = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus GbmMesaAllocator::getIMapperLibrarySuffix(std::string* outResult) {
    *outResult = "gm";
    return ndk::ScopedAStatus::ok();
}

::ndk::SpAIBinder GbmMesaAllocator::createBinder() {
    auto binder = BnAllocator::createBinder();
    AIBinder_setInheritRt(binder.get(), true);
    return binder;
}

}  // namespace aidl::android::hardware::graphics::allocator::impl
