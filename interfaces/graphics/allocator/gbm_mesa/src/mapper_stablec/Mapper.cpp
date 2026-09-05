/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#ifndef GRALLOC_GBM_MESA_STABLEC_MAPPER_MAPPER_CPP_
#define GRALLOC_GBM_MESA_STABLEC_MAPPER_MAPPER_CPP_

#include "Mapper.h"

#define LOG_TAG "mapper.gm"

#include <aidl/android/hardware/graphics/allocator/BufferDescriptorInfo.h>
#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <aidl/android/hardware/graphics/common/StandardMetadataType.h>
#include <android-base/unique_fd.h>
#include <android/hardware/graphics/mapper/IMapper.h>
#include <android/hardware/graphics/mapper/utils/IMapperMetadataTypes.h>
#include <android/hardware/graphics/mapper/utils/IMapperProvider.h>
#include <cutils/native_handle.h>
#include <gralloctypes/Gralloc4.h>
#include <mutex>
#include <unordered_map>

#include "gralloc_gbm_mesa.h"
#include "gbm_mesa_loader.h"
#include "log.h"

using aidl::android::hardware::graphics::common::BlendMode;
using aidl::android::hardware::graphics::common::Dataspace;
using aidl::android::hardware::graphics::common::Cta861_3;
using aidl::android::hardware::graphics::common::Smpte2086;

using namespace aidl::android::hardware::graphics::common;
using namespace android::hardware::graphics::mapper;
using aidl::android::hardware::graphics::allocator::BufferDescriptorInfo;
using android::base::unique_fd;

#define REQUIRE_DRIVER()                                           \
    if (!mInitialized) {                                           \
        log_e("Failed to %s. Driver is uninitialized.", __func__); \
        return AIMAPPER_ERROR_NO_RESOURCES;                        \
    }

#define VALIDATE_BUFFER_HANDLE(bufferHandle)                    \
    if (!(bufferHandle)) {                                      \
        log_e("Failed to %s. Null buffer_handle_t.", __func__); \
        return AIMAPPER_ERROR_BAD_BUFFER;                       \
    }

#define VALIDATE_DRIVER_AND_BUFFER_HANDLE(bufferHandle) \
    REQUIRE_DRIVER()                                    \
    VALIDATE_BUFFER_HANDLE(bufferHandle)

constexpr const char* STANDARD_METADATA_NAME =
        "android.hardware.graphics.common.StandardMetadataType";

static bool isStandardMetadata(AIMapper_MetadataType metadataType) {
    return strcmp(STANDARD_METADATA_NAME, metadataType.name) == 0;
}

int getPlaneLayouts(uint32_t gbmFormat, std::vector<PlaneLayout>* outPlaneLayouts);

// We store the all of metadata with a K,V map [int, struct gralloc_metadata] named gralloc_metadata_prime_fd_map.
static std::unordered_map<int, struct gralloc_metadata *> gralloc_metadata_prime_fd_map;

// Use the existing metadata or create it if it doesn't exist.
static int createMetadata(gralloc_handle_t *handle, struct gralloc_metadata **outMetadata) {
    auto it = gralloc_metadata_prime_fd_map.find(handle->prime_fd);
    gralloc_metadata_t *md = (it != gralloc_metadata_prime_fd_map.end()) ? it->second : nullptr;
    if (!md) {
        log_d("Could not found metadata for handle (fd=%d), create new one.", handle->prime_fd);
        md = new gralloc_metadata {
            .prime_fd = handle->prime_fd,
            .blend_mode = BlendMode::NONE,
            .dataspace = Dataspace::UNKNOWN,
            .cta861_3 = {},
            .smpte2086 = {},
        };
        gralloc_metadata_prime_fd_map[handle->prime_fd] = md;
    }
    *outMetadata = md;
    return 0;
}

class GbmMesaMapperV5 final : public vendor::mapper::IMapperV5Impl {
  private:
    bool mInitialized = false;

  public:
    GbmMesaMapperV5() {
        if (gralloc_gbm_device_init() > 0) {
            mInitialized = true;
        } else {
            log_e("Failed to initialize GBM device (Mapper V5)");
        }
    }

    ~GbmMesaMapperV5() override = default;

    AIMapper_Error importBuffer(const native_handle_t* _Nonnull handle,
                                buffer_handle_t _Nullable* _Nonnull outBufferHandle) override;

    AIMapper_Error freeBuffer(buffer_handle_t _Nonnull buffer) override;

    AIMapper_Error getTransportSize(buffer_handle_t _Nonnull buffer, uint32_t* _Nonnull outNumFds,
                                    uint32_t* _Nonnull outNumInts) override;

    AIMapper_Error lock(buffer_handle_t _Nonnull buffer, uint64_t cpuUsage, ARect accessRegion,
                        int acquireFence, void* _Nullable* _Nonnull outData) override;

    AIMapper_Error unlock(buffer_handle_t _Nonnull buffer, int* _Nonnull releaseFence) override;

    AIMapper_Error flushLockedBuffer(buffer_handle_t _Nonnull buffer) override;

    AIMapper_Error rereadLockedBuffer(buffer_handle_t _Nonnull buffer) override;

    int32_t getMetadata(buffer_handle_t _Nonnull buffer, AIMapper_MetadataType metadataType,
                        void* _Nonnull outData, size_t outDataSize) override;

    int32_t getStandardMetadata(buffer_handle_t _Nonnull buffer, int64_t standardMetadataType,
                                void* _Nonnull outData, size_t outDataSize) override;

    AIMapper_Error setMetadata(const native_handle_t* buffer, AIMapper_MetadataType metadataType,
                               const void* _Nonnull metadata, size_t metadataSize) override;

    AIMapper_Error setStandardMetadata(buffer_handle_t _Nonnull buffer,
                                       int64_t standardMetadataType, const void* _Nonnull metadata,
                                       size_t metadataSize) override;

    AIMapper_Error listSupportedMetadataTypes(
            const AIMapper_MetadataTypeDescription* _Nullable* _Nonnull outDescriptionList,
            size_t* _Nonnull outNumberOfDescriptions) override;

    AIMapper_Error dumpBuffer(buffer_handle_t _Nonnull bufferHandle,
                              AIMapper_DumpBufferCallback _Nonnull dumpBufferCallback,
                              void* _Null_unspecified context) override;

    AIMapper_Error dumpAllBuffers(AIMapper_BeginDumpBufferCallback _Nonnull beginDumpBufferCallback,
                                  AIMapper_DumpBufferCallback _Nonnull dumpBufferCallback,
                                  void* _Null_unspecified context) override;

    AIMapper_Error getReservedRegion(buffer_handle_t _Nonnull buffer,
                                     void* _Nullable* _Nonnull outReservedRegion,
                                     uint64_t* _Nonnull outReservedSize) override;

  private:
    template <typename F, StandardMetadataType TYPE>
    int32_t getStandardMetadata(buffer_handle_t handle, F&& provide,
                                StandardMetadata<TYPE>);

    template <StandardMetadataType TYPE>
    AIMapper_Error setStandardMetadata(buffer_handle_t handle,
                                       typename StandardMetadata<TYPE>::value_type&& value);

    void dumpBuffer(
            buffer_handle_t handle,
            std::function<void(AIMapper_MetadataType, const std::vector<uint8_t>&)> callback);
};

void GbmMesaMapperV5::dumpBuffer(
        buffer_handle_t handle,
        std::function<void(AIMapper_MetadataType, const std::vector<uint8_t>&)> callback) {
    // TODO: Finish this function
}

AIMapper_Error GbmMesaMapperV5::importBuffer(
        const native_handle_t* _Nonnull bufferHandle,
        buffer_handle_t _Nullable* _Nonnull outBufferHandle) {
    REQUIRE_DRIVER()

    if (!bufferHandle || bufferHandle->numFds == 0) {
        log_e("Failed to importBuffer. Bad handle.");
        return AIMAPPER_ERROR_BAD_BUFFER;
    }

    native_handle_t* importedBufferHandle = native_handle_clone(bufferHandle);
    if (!importedBufferHandle) {
        log_e("Failed to importBuffer. Handle clone failed: %s.", strerror(errno));
        return AIMAPPER_ERROR_NO_RESOURCES;
    }

    // Import buffer into GBM
    log_i("Importing buffer to GBM...");
    int ret = gralloc_gm_buffer_import(importedBufferHandle);
    if (ret) {
        log_i("do gralloc_gm_buffer_import failed, ret=%d", ret);
        native_handle_close(importedBufferHandle);
        native_handle_delete(importedBufferHandle);
        return AIMAPPER_ERROR_NO_RESOURCES;
    }

    *outBufferHandle = importedBufferHandle;
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::freeBuffer(buffer_handle_t _Nonnull buffer) {
    VALIDATE_DRIVER_AND_BUFFER_HANDLE(buffer)
    gralloc_handle_t *hnd = gralloc_handle(buffer);

    auto it_meta = gralloc_metadata_prime_fd_map.find(hnd->prime_fd);
    if (it_meta != gralloc_metadata_prime_fd_map.end()) {
        delete it_meta->second;
        gralloc_metadata_prime_fd_map.erase(it_meta);
    }
    int ret = gralloc_gm_buffer_free(buffer);
    if (ret) {
        return AIMAPPER_ERROR_BAD_BUFFER;
    }

    native_handle_close(buffer);
    native_handle_delete(const_cast<native_handle_t*>(buffer));
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::getTransportSize(buffer_handle_t _Nonnull bufferHandle,
                                                     uint32_t* _Nonnull outNumFds,
                                                     uint32_t* _Nonnull outNumInts) {
    VALIDATE_DRIVER_AND_BUFFER_HANDLE(bufferHandle)
    *outNumFds = bufferHandle->numFds;
    *outNumInts = bufferHandle->numInts;
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::lock(buffer_handle_t _Nonnull bufferHandle, uint64_t cpuUsage,
                                         ARect region, int acquireFenceRawFd,
                                         void* _Nullable* _Nonnull outData) {
    unique_fd acquireFence(acquireFenceRawFd);
    VALIDATE_DRIVER_AND_BUFFER_HANDLE(bufferHandle)
    
    if (cpuUsage == 0) {
        log_e("Failed to lock. Bad cpu usage: %" PRIu64 ".", cpuUsage);
        return AIMAPPER_ERROR_BAD_VALUE;
    }

    int usage = static_cast<int>(cpuUsage);
    int ret = gralloc_gbm_bo_lock(bufferHandle, usage, 
                                 region.left, region.top, 
                                 region.right - region.left, 
                                 region.bottom - region.top, 
                                 outData);
    if (ret) {
        log_e("Failed to lock buffer: %d", ret);
        return AIMAPPER_ERROR_BAD_VALUE;
    }

    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::unlock(buffer_handle_t _Nonnull buffer,
                                           int* _Nonnull releaseFence) {
    VALIDATE_DRIVER_AND_BUFFER_HANDLE(buffer)
    int ret = gralloc_gbm_bo_unlock(buffer);
    if (ret) {
        log_e("Failed to unlock buffer: %d", ret);
        return AIMAPPER_ERROR_BAD_BUFFER;
    }
    
    *releaseFence = -1; // Fences not supported
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::flushLockedBuffer(buffer_handle_t _Nonnull buffer) {
    log_w("flushLockedBuffer() required, but not implemented");
    // No-op for GBM Mesa
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::rereadLockedBuffer(buffer_handle_t _Nonnull buffer) {
    log_w("rereadLockedBuffer() required, but not implemented");
    // No-op for GBM Mesa
    return AIMAPPER_ERROR_NONE;
}

int32_t GbmMesaMapperV5::getMetadata(buffer_handle_t _Nonnull buffer,
                                         AIMapper_MetadataType metadataType, void* _Nonnull outData,
                                         size_t outDataSize) {
    if (isStandardMetadata(metadataType)) {
        return getStandardMetadata(buffer, metadataType.value, outData, outDataSize);
    }
    return AIMAPPER_ERROR_UNSUPPORTED;
}

int32_t GbmMesaMapperV5::getStandardMetadata(buffer_handle_t _Nonnull bufferHandle,
                                                 int64_t standardType, void* _Nonnull outData,
                                                 size_t outDataSize) {
    REQUIRE_DRIVER()
    VALIDATE_BUFFER_HANDLE(bufferHandle)

    gralloc_handle_t* handle = gralloc_handle(bufferHandle);
    if (!handle) {
        log_e("Failed to get gralloc handle");
        return AIMAPPER_ERROR_BAD_BUFFER;
    }

    auto provider = [&]<StandardMetadataType T>(auto&& provide) -> int32_t {
        return getStandardMetadata(bufferHandle, provide, StandardMetadata<T>{});
    };
    
    return provideStandardMetadata(static_cast<StandardMetadataType>(standardType), 
                                  outData, outDataSize, provider);
}

template <typename F, StandardMetadataType metadataType>
int32_t GbmMesaMapperV5::getStandardMetadata(buffer_handle_t handle, F&& provide,
                                                 StandardMetadata<metadataType>) {
    gralloc_handle_t *hnd = gralloc_handle(handle);
    gralloc_metadata_t *metadata = nullptr;

    if (!hnd) return AIMAPPER_ERROR_BAD_BUFFER;
    
    createMetadata(hnd, &metadata);
    if (!metadata) return AIMAPPER_ERROR_NO_RESOURCES;

    if constexpr (metadataType == StandardMetadataType::BUFFER_ID) {
        return provide(reinterpret_cast<uint64_t>(handle));
    }
    if constexpr (metadataType == StandardMetadataType::WIDTH) {
        return provide(static_cast<int32_t>(hnd->width));
    }
    if constexpr (metadataType == StandardMetadataType::HEIGHT) {
        return provide(static_cast<int32_t>(hnd->height));
    }
    if constexpr (metadataType == StandardMetadataType::LAYER_COUNT) {
        return provide(1);
    }
    if constexpr (metadataType == StandardMetadataType::PIXEL_FORMAT_REQUESTED) {
        return provide(static_cast<PixelFormat>(hnd->format));
    }
    if constexpr (metadataType == StandardMetadataType::PIXEL_FORMAT_FOURCC) {
        auto forcc_format = static_cast<uint32_t>(gralloc_gm_android_format_to_gbm_format(hnd->format));
        if (forcc_format > 0)
            return provide(forcc_format);
        return AIMAPPER_ERROR_UNSUPPORTED;
    }
    if constexpr (metadataType == StandardMetadataType::PIXEL_FORMAT_MODIFIER) {
        return provide(hnd->modifier);
    }
    if constexpr (metadataType == StandardMetadataType::USAGE) {
        return provide(static_cast<BufferUsage>(hnd->usage));
    }
    if constexpr (metadataType == StandardMetadataType::ALLOCATION_SIZE) {
        size_t size = 0;
        struct gbm_bo* bo = gralloc_get_gbm_bo_from_handle(handle);
        if (bo) size = gbm_priv_ops.gbm_bo_get_stride(bo) * gbm_priv_ops.gbm_bo_get_height(bo);
        return provide(static_cast<uint64_t>(size));
    }
    if constexpr (metadataType == StandardMetadataType::PROTECTED_CONTENT) {
        uint64_t hasProtectedContent =
                (hnd->usage & static_cast<int64_t>(BufferUsage::PROTECTED)) ? 1 : 0;
        return provide(hasProtectedContent);
    }
    if constexpr (metadataType == StandardMetadataType::COMPRESSION) {
        return provide(android::gralloc4::Compression_None);
    }
    if constexpr (metadataType == StandardMetadataType::INTERLACED) {
        return provide(android::gralloc4::Interlaced_None);
    }
    if constexpr (metadataType == StandardMetadataType::CHROMA_SITING) {
        return provide(android::gralloc4::ChromaSiting_None);
    }
    if constexpr (metadataType == StandardMetadataType::PLANE_LAYOUTS) {
        std::vector<PlaneLayout> planeLayouts;
        getPlaneLayouts(gralloc_gm_android_format_to_gbm_format(hnd->format), &planeLayouts);

        for (size_t plane = 0; plane < planeLayouts.size(); plane++) {
            PlaneLayout& planeLayout = planeLayouts[plane];
            planeLayout.offsetInBytes = 0;
            planeLayout.strideInBytes = hnd->stride;
            // FIXME: vertical_subsampling=1 for now
            planeLayout.totalSizeInBytes = hnd->stride * DIV_ROUND_UP(hnd->height, 1);
            planeLayout.widthInSamples =
                    hnd->width / planeLayout.horizontalSubsampling;
            planeLayout.heightInSamples =
                    hnd->height / planeLayout.verticalSubsampling;
        }

        return provide(planeLayouts);
    }
    if constexpr (metadataType == StandardMetadataType::CROP) {
        const uint32_t numPlanes = 1; // We only support 1 currently
        const uint32_t w = hnd->width;
        const uint32_t h = hnd->height;
        std::vector<aidl::android::hardware::graphics::common::Rect> crops;
        for (uint32_t plane = 0; plane < numPlanes; plane++) {
            aidl::android::hardware::graphics::common::Rect crop;
            crop.left = 0;
            crop.top = 0;
            crop.right = w;
            crop.bottom = h;
            crops.push_back(crop);
        }

        return provide(crops);
    }
    if constexpr (metadataType == StandardMetadataType::DATASPACE) {
        return provide(metadata->dataspace);
    }
    if constexpr (metadataType == StandardMetadataType::BLEND_MODE) {
        return provide(metadata->blend_mode);
    }
    if constexpr (metadataType == StandardMetadataType::SMPTE2086) {
        return provide(metadata->smpte2086);
    }
    if constexpr (metadataType == StandardMetadataType::CTA861_3) {
        return provide(metadata->cta861_3);
    }
    if constexpr (metadataType == StandardMetadataType::STRIDE) {
        // This stride should be the same value of AllocationResult of Allocator.
        // This stride will be used in validateBufferSize(), and unit is pixels.
        return provide(static_cast<int32_t>(gralloc_gm_android_caculate_pixel_stride(hnd->format, hnd->stride)));
    }

    return AIMAPPER_ERROR_UNSUPPORTED;
}

AIMapper_Error GbmMesaMapperV5::setMetadata(const native_handle_t* buffer,
                                            AIMapper_MetadataType metadataType,
                                            const void* _Nonnull metadata,
                                            size_t metadataSize) {
    if (!isStandardMetadata(metadataType)) {
        return AIMAPPER_ERROR_UNSUPPORTED; // It is acceptable to non-standard metadata type.
    }

    buffer_handle_t bufferHandle = reinterpret_cast<buffer_handle_t>(buffer);
    return setStandardMetadata(bufferHandle, metadataType.value, metadata, metadataSize);
}

AIMapper_Error GbmMesaMapperV5::setStandardMetadata(buffer_handle_t _Nonnull buffer,
                                                    int64_t standardMetadataType,
                                                    const void* _Nonnull metadata,
                                                    size_t metadataSize) {
    REQUIRE_DRIVER()
    VALIDATE_BUFFER_HANDLE(buffer)

    gralloc_handle_t* handle = gralloc_handle(buffer);
    if (!handle) {
        log_e("Failed to get gralloc handle");
        return AIMAPPER_ERROR_BAD_BUFFER;
    }

    gralloc_metadata_t* grallocMetadata = nullptr;
    if (createMetadata(handle, &grallocMetadata) != 0 || !grallocMetadata) {
        log_e("Failed to create or retrieve metadata for buffer (fd=%d)", handle->prime_fd);
        return AIMAPPER_ERROR_NO_RESOURCES;
    }

    StandardMetadataType metadataTypeEnum = static_cast<StandardMetadataType>(standardMetadataType);
    std::string metadataTypeName = toString(metadataTypeEnum);
    log_v("Setting metadata type: %s (%ld), size: %zu", metadataTypeName.c_str(), standardMetadataType, metadataSize);

    switch (metadataTypeEnum) {
        case StandardMetadataType::DATASPACE: {
            if (metadataSize != sizeof(Dataspace)) return AIMAPPER_ERROR_BAD_VALUE;
            const auto* value = static_cast<const Dataspace*>(metadata);
            grallocMetadata->dataspace = *value;
            log_d("Set dataspace to %d for handle (fd = %d)", static_cast<int>(*value), handle->prime_fd);
            return AIMAPPER_ERROR_NONE;
        }
        case StandardMetadataType::BLEND_MODE: {
            if (metadataSize != sizeof(BlendMode)) return AIMAPPER_ERROR_BAD_VALUE;
            const auto* value = static_cast<const BlendMode*>(metadata);
            grallocMetadata->blend_mode = *value;
            log_d("Set blend_mode to %d for handle (fd = %d)", static_cast<int>(*value), handle->prime_fd);
            return AIMAPPER_ERROR_NONE;
        }
        case StandardMetadataType::SMPTE2086: {
            if (metadataSize != sizeof(Smpte2086)) return AIMAPPER_ERROR_BAD_VALUE;
            const auto* value = static_cast<const Smpte2086*>(metadata);
            grallocMetadata->smpte2086 = *value;
            return AIMAPPER_ERROR_NONE;
        }
        case StandardMetadataType::CTA861_3: {
            if (metadataSize != sizeof(Cta861_3)) return AIMAPPER_ERROR_BAD_VALUE;
            const auto* value = static_cast<const Cta861_3*>(metadata);
            grallocMetadata->cta861_3 = *value;
            return AIMAPPER_ERROR_NONE;
        }

        // Read-Only types
        case StandardMetadataType::BUFFER_ID:
        case StandardMetadataType::NAME:
        case StandardMetadataType::WIDTH:
        case StandardMetadataType::HEIGHT:
        case StandardMetadataType::LAYER_COUNT:
        case StandardMetadataType::PIXEL_FORMAT_REQUESTED:
        case StandardMetadataType::USAGE:
            log_w("Metadata type %s is read-only", metadataTypeName.c_str());
            return AIMAPPER_ERROR_BAD_VALUE;

        default:
            log_i("Metadata type %s is not supported for set", metadataTypeName.c_str());
            return AIMAPPER_ERROR_UNSUPPORTED;
    }
}

constexpr AIMapper_MetadataTypeDescription describeStandard(StandardMetadataType type,
                                                            bool isGettable, bool isSettable) {
    return {{STANDARD_METADATA_NAME, static_cast<int64_t>(type)},
            nullptr,
            isGettable,
            isSettable,
            {0}};
}

AIMapper_Error GbmMesaMapperV5::listSupportedMetadataTypes(
        const AIMapper_MetadataTypeDescription* _Nullable* _Nonnull outDescriptionList,
        size_t* _Nonnull outNumberOfDescriptions) {
    static constexpr std::array<AIMapper_MetadataTypeDescription, 10> sSupportedMetadataTypes{
        describeStandard(StandardMetadataType::BUFFER_ID, true, false),
        describeStandard(StandardMetadataType::NAME, false, false),
        describeStandard(StandardMetadataType::WIDTH, true, false),
        describeStandard(StandardMetadataType::HEIGHT, true, false),
        describeStandard(StandardMetadataType::LAYER_COUNT, true, false),
        describeStandard(StandardMetadataType::PIXEL_FORMAT_REQUESTED, true, false),
        describeStandard(StandardMetadataType::USAGE, true, false),
        describeStandard(StandardMetadataType::ALLOCATION_SIZE, true, false),
        describeStandard(StandardMetadataType::STRIDE, true, false),
        describeStandard(StandardMetadataType::PROTECTED_CONTENT, false, false)
    };
    *outDescriptionList = sSupportedMetadataTypes.data();
    *outNumberOfDescriptions = sSupportedMetadataTypes.size();
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::dumpBuffer(
        buffer_handle_t _Nonnull bufferHandle,
        AIMapper_DumpBufferCallback _Nonnull dumpBufferCallback, void* context) {
    VALIDATE_DRIVER_AND_BUFFER_HANDLE(bufferHandle)
    
    auto callback = [&](AIMapper_MetadataType type, const std::vector<uint8_t>& buffer) {
        dumpBufferCallback(context, type, buffer.data(), buffer.size());
    };
    
    dumpBuffer(bufferHandle, callback);
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::dumpAllBuffers(
        AIMapper_BeginDumpBufferCallback _Nonnull beginDumpBufferCallback,
        AIMapper_DumpBufferCallback _Nonnull dumpBufferCallback, void* context) {
    REQUIRE_DRIVER()
    
    auto callback = [&](AIMapper_MetadataType type, const std::vector<uint8_t>& buffer) {
        dumpBufferCallback(context, type, buffer.data(), buffer.size());
    };

    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error GbmMesaMapperV5::getReservedRegion(buffer_handle_t _Nonnull buffer,
                                                      void* _Nullable* _Nonnull outReservedRegion,
                                                      uint64_t* _Nonnull outReservedSize) {
    VALIDATE_DRIVER_AND_BUFFER_HANDLE(buffer)
    *outReservedRegion = nullptr;
    *outReservedSize = 0;
    return AIMAPPER_ERROR_NONE; // Not supported
}

extern "C" uint32_t ANDROID_HAL_MAPPER_VERSION = AIMAPPER_VERSION_5;

extern "C" AIMapper_Error AIMapper_loadIMapper(AIMapper* _Nullable* _Nonnull outImplementation) {
    static vendor::mapper::IMapperProvider<GbmMesaMapperV5> provider;
    return provider.load(outImplementation);
}

const std::unordered_map<uint32_t, std::vector<PlaneLayout>>& GetPlaneLayoutsMap() {
    static const auto* kPlaneLayoutsMap =
            new std::unordered_map<uint32_t, std::vector<PlaneLayout>>({
                    {GBM_FORMAT_ABGR8888,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 8,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 16,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_A,
                                             .offsetInBits = 24,
                                             .sizeInBits = 8}},
                             .sampleIncrementInBits = 32,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_ABGR2101010,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 10},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 10,
                                             .sizeInBits = 10},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 20,
                                             .sizeInBits = 10},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_A,
                                             .offsetInBits = 30,
                                             .sizeInBits = 2}},
                             .sampleIncrementInBits = 32,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_ABGR16161616F,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 16},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 16,
                                             .sizeInBits = 16},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 32,
                                             .sizeInBits = 16},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_A,
                                             .offsetInBits = 48,
                                             .sizeInBits = 16}},
                             .sampleIncrementInBits = 64,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_ARGB8888,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 0,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 8,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 16,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_A,
                                             .offsetInBits = 24,
                                             .sizeInBits = 8}},
                             .sampleIncrementInBits = 32,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_NV12,
                     {{
                              .components = {{.type = android::gralloc4::PlaneLayoutComponentType_Y,
                                              .offsetInBits = 0,
                                              .sizeInBits = 8}},
                              .sampleIncrementInBits = 8,
                              .horizontalSubsampling = 1,
                              .verticalSubsampling = 1,
                      },
                      {
                              .components =
                                      {{.type = android::gralloc4::PlaneLayoutComponentType_CB,
                                        .offsetInBits = 0,
                                        .sizeInBits = 8},
                                       {.type = android::gralloc4::PlaneLayoutComponentType_CR,
                                        .offsetInBits = 8,
                                        .sizeInBits = 8}},
                              .sampleIncrementInBits = 16,
                              .horizontalSubsampling = 2,
                              .verticalSubsampling = 2,
                      }}},

                    {GBM_FORMAT_NV21,
                     {{
                              .components = {{.type = android::gralloc4::PlaneLayoutComponentType_Y,
                                              .offsetInBits = 0,
                                              .sizeInBits = 8}},
                              .sampleIncrementInBits = 8,
                              .horizontalSubsampling = 1,
                              .verticalSubsampling = 1,
                      },
                      {
                              .components =
                                      {{.type = android::gralloc4::PlaneLayoutComponentType_CR,
                                        .offsetInBits = 0,
                                        .sizeInBits = 8},
                                       {.type = android::gralloc4::PlaneLayoutComponentType_CB,
                                        .offsetInBits = 8,
                                        .sizeInBits = 8}},
                              .sampleIncrementInBits = 16,
                              .horizontalSubsampling = 2,
                              .verticalSubsampling = 2,
                      }}},

                    {GBM_FORMAT_R8,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 8}},
                             .sampleIncrementInBits = 8,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_R16,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 16}},
                             .sampleIncrementInBits = 16,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_RGB565,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 0,
                                             .sizeInBits = 5},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 5,
                                             .sizeInBits = 6},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 11,
                                             .sizeInBits = 5}},
                             .sampleIncrementInBits = 16,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_BGR888,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 8,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 16,
                                             .sizeInBits = 8}},
                             .sampleIncrementInBits = 24,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_XBGR8888,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 8,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 16,
                                             .sizeInBits = 8}},
                             .sampleIncrementInBits = 32,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_YVU420,
                     {
                             {
                                     .components = {{.type = android::gralloc4::
                                                             PlaneLayoutComponentType_Y,
                                                     .offsetInBits = 0,
                                                     .sizeInBits = 8}},
                                     .sampleIncrementInBits = 8,
                                     .horizontalSubsampling = 1,
                                     .verticalSubsampling = 1,
                             },
                             {
                                     .components = {{.type = android::gralloc4::
                                                             PlaneLayoutComponentType_CR,
                                                     .offsetInBits = 0,
                                                     .sizeInBits = 8}},
                                     .sampleIncrementInBits = 8,
                                     .horizontalSubsampling = 2,
                                     .verticalSubsampling = 2,
                             },
                             {
                                     .components = {{.type = android::gralloc4::
                                                             PlaneLayoutComponentType_CB,
                                                     .offsetInBits = 0,
                                                     .sizeInBits = 8}},
                                     .sampleIncrementInBits = 8,
                                     .horizontalSubsampling = 2,
                                     .verticalSubsampling = 2,
                             },
                     }},

                    {GBM_FORMAT_RGBX8888,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 16,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 8,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 8}},
                             .sampleIncrementInBits = 32,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    {GBM_FORMAT_XRGB8888,
                     {{
                             .components = {{.type = android::gralloc4::PlaneLayoutComponentType_B,
                                             .offsetInBits = 16,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_G,
                                             .offsetInBits = 8,
                                             .sizeInBits = 8},
                                            {.type = android::gralloc4::PlaneLayoutComponentType_R,
                                             .offsetInBits = 0,
                                             .sizeInBits = 8}},
                             .sampleIncrementInBits = 32,
                             .horizontalSubsampling = 1,
                             .verticalSubsampling = 1,
                     }}},

                    // TODO: Add support for more pixel format.
            });
    return *kPlaneLayoutsMap;
}

int getPlaneLayouts(uint32_t gbmFormat, std::vector<PlaneLayout>* outPlaneLayouts) {
    const auto& planeLayoutsMap = GetPlaneLayoutsMap();
    const auto it = planeLayoutsMap.find(gbmFormat);
    if (it == planeLayoutsMap.end()) {
        log_e("Unknown plane layout for format %d", gbmFormat);
        return -EINVAL;
    }

    *outPlaneLayouts = it->second;
    return 0;
}

#endif // GRALLOC_GBM_MESA_STABLEC_MAPPER_MAPPER_CPP_