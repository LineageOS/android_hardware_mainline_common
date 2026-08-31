/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-mapper"

#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/StandardMetadataType.h>
#include <android-base/unique_fd.h>
#include <android/hardware/graphics/mapper/IMapper.h>
#include <android/hardware/graphics/mapper/utils/IMapperMetadataTypes.h>
#include <android/hardware/graphics/mapper/utils/IMapperProvider.h>
#include <drm/drm_fourcc.h>
#include <gralloctypes/Gralloc4.h>
#include <log/log.h>
#include <sync/sync.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Buffer.h"

namespace {

using aidl::android::hardware::graphics::common::BlendMode;
using aidl::android::hardware::graphics::common::BufferUsage;
using aidl::android::hardware::graphics::common::Cta861_3;
using aidl::android::hardware::graphics::common::Dataspace;
using aidl::android::hardware::graphics::common::ExtendableType;
using aidl::android::hardware::graphics::common::Rect;
using aidl::android::hardware::graphics::common::Smpte2086;
using aidl::android::hardware::graphics::common::StandardMetadataType;
using android::base::unique_fd;
using namespace android::hardware::graphics::mapper;

constexpr char kStandardMetadataName[] = "android.hardware.graphics.common.StandardMetadataType";
constexpr uint64_t kCpuUsageMask = 0xff;
constexpr int kFenceTimeoutMs = 3000;

constexpr AIMapper_MetadataTypeDescription Describe(StandardMetadataType type, bool settable) {
    return {{kStandardMetadataName, static_cast<int64_t>(type)}, nullptr, true, settable, {0}};
}

struct ImportedBuffer {
    native_handle_t* handle = nullptr;
    fb::HandleView view;
    void* pixels = MAP_FAILED;
    fb::SharedMetadata* metadata = nullptr;
    size_t metadata_size = 0;
    uint32_t lock_count = 0;

    ~ImportedBuffer() {
        if (pixels != MAP_FAILED) munmap(pixels, view.layout.allocation_size);
        if (metadata != nullptr) munmap(metadata, metadata_size);
        if (handle != nullptr) {
            native_handle_close(handle);
            native_handle_delete(handle);
        }
    }
};

class MetadataFileLock {
  public:
    MetadataFileLock(int fd, int operation) : fd_(fd), locked_(flock(fd, operation) == 0) {}
    ~MetadataFileLock() {
        if (locked_) flock(fd_, LOCK_UN);
    }
    bool locked() const { return locked_; }

  private:
    int fd_;
    bool locked_;
};

class Mapper final : public vendor::mapper::IMapperV5Impl {
  public:
    AIMapper_Error importBuffer(const native_handle_t* handle,
                                buffer_handle_t* out_buffer) override;
    AIMapper_Error freeBuffer(buffer_handle_t buffer) override;
    AIMapper_Error getTransportSize(buffer_handle_t buffer, uint32_t* fds, uint32_t* ints) override;
    AIMapper_Error lock(buffer_handle_t buffer, uint64_t cpu_usage, ARect region, int acquire_fence,
                        void** data) override;
    AIMapper_Error unlock(buffer_handle_t buffer, int* release_fence) override;
    AIMapper_Error flushLockedBuffer(buffer_handle_t buffer) override;
    AIMapper_Error rereadLockedBuffer(buffer_handle_t buffer) override;
    int32_t getMetadata(buffer_handle_t buffer, AIMapper_MetadataType type, void* data,
                        size_t size) override;
    int32_t getStandardMetadata(buffer_handle_t buffer, int64_t type, void* data,
                                size_t size) override;
    AIMapper_Error setMetadata(buffer_handle_t buffer, AIMapper_MetadataType type, const void* data,
                               size_t size) override;
    AIMapper_Error setStandardMetadata(buffer_handle_t buffer, int64_t type, const void* data,
                                       size_t size) override;
    AIMapper_Error listSupportedMetadataTypes(const AIMapper_MetadataTypeDescription** descriptions,
                                              size_t* count) override;
    AIMapper_Error dumpBuffer(buffer_handle_t buffer, AIMapper_DumpBufferCallback callback,
                              void* context) override;
    AIMapper_Error dumpAllBuffers(AIMapper_BeginDumpBufferCallback begin,
                                  AIMapper_DumpBufferCallback callback, void* context) override;
    AIMapper_Error getReservedRegion(buffer_handle_t buffer, void** region,
                                     uint64_t* size) override;

  private:
    std::shared_ptr<ImportedBuffer> Find(buffer_handle_t buffer);
    int32_t Get(const ImportedBuffer& buffer, StandardMetadataType type, void* data, size_t size);
    AIMapper_Error Set(ImportedBuffer* buffer, StandardMetadataType type, const void* data,
                       size_t size);
    void Dump(const std::shared_ptr<ImportedBuffer>& buffer, AIMapper_DumpBufferCallback callback,
              void* context);

    std::mutex mutex_;
    std::mutex metadata_mutex_;
    std::map<buffer_handle_t, std::shared_ptr<ImportedBuffer>> buffers_;
};

std::shared_ptr<ImportedBuffer> Mapper::Find(buffer_handle_t buffer) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buffer);
    return it == buffers_.end() ? nullptr : it->second;
}

AIMapper_Error Mapper::importBuffer(const native_handle_t* handle, buffer_handle_t* out_buffer) {
    if (out_buffer == nullptr) return AIMAPPER_ERROR_BAD_VALUE;
    *out_buffer = nullptr;
    fb::HandleView view;
    std::string reason;
    if (!fb::ValidateHandle(handle, &view, &reason)) {
        ALOGE("Rejected buffer import: %s", reason.c_str());
        return AIMAPPER_ERROR_BAD_BUFFER;
    }
    native_handle_t* clone = native_handle_clone(handle);
    if (clone == nullptr) return AIMAPPER_ERROR_NO_RESOURCES;
    auto imported = std::make_shared<ImportedBuffer>();
    imported->handle = clone;
    if (!fb::ValidateHandle(clone, &imported->view, &reason)) return AIMAPPER_ERROR_BAD_BUFFER;
    imported->metadata_size = sizeof(fb::SharedMetadata) + imported->view.layout.reserved_size;
    void* metadata = mmap(nullptr, imported->metadata_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                          imported->view.metadata_fd, 0);
    if (metadata == MAP_FAILED) return AIMAPPER_ERROR_NO_RESOURCES;
    imported->metadata = static_cast<fb::SharedMetadata*>(metadata);
    if (imported->metadata->magic != fb::kMetadataMagic ||
        imported->metadata->abi_version != fb::kAbiVersion ||
        imported->metadata->allocation_id != imported->view.allocation_id ||
        imported->metadata->metadata_size != imported->metadata_size ||
        imported->metadata->reserved_size != imported->view.layout.reserved_size) {
        ALOGE("Rejected buffer with inconsistent shared metadata");
        return AIMAPPER_ERROR_BAD_BUFFER;
    }
    imported->view.layout.name = imported->metadata->name;
    {
        std::lock_guard lock(mutex_);
        buffers_.emplace(clone, imported);
    }
    *out_buffer = clone;
    ALOGV("Imported id=%" PRIu64 " %ux%u %s", imported->view.allocation_id,
          imported->view.layout.width, imported->view.layout.height,
          fb::FormatName(imported->view.layout.format).c_str());
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::freeBuffer(buffer_handle_t buffer) {
    std::shared_ptr<ImportedBuffer> removed;
    {
        std::lock_guard lock(mutex_);
        auto it = buffers_.find(buffer);
        if (it == buffers_.end()) return AIMAPPER_ERROR_BAD_BUFFER;
        if (it->second->lock_count != 0) {
            ALOGW("Freeing locked buffer id=%" PRIu64, it->second->view.allocation_id);
        }
        removed = std::move(it->second);
        buffers_.erase(it);
    }
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::getTransportSize(buffer_handle_t buffer, uint32_t* fds, uint32_t* ints) {
    if (fds == nullptr || ints == nullptr || Find(buffer) == nullptr)
        return AIMAPPER_ERROR_BAD_BUFFER;
    *fds = fb::kHandleFds;
    *ints = fb::kHandleInts;
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::lock(buffer_handle_t buffer, uint64_t cpu_usage, ARect region,
                            int acquire_fence, void** data) {
    unique_fd fence(acquire_fence);
    if (data == nullptr) return AIMAPPER_ERROR_BAD_VALUE;
    *data = nullptr;
    auto imported = Find(buffer);
    if (imported == nullptr) return AIMAPPER_ERROR_BAD_BUFFER;
    const uint64_t read_usage = cpu_usage & 0xf;
    const uint64_t write_usage = cpu_usage & 0xf0;
    const uint64_t read_mode = read_usage & 0x3;
    const uint64_t write_mode = (write_usage >> 4) & 0x3;
    const bool valid_read = read_usage == 0 || read_mode == 2 || read_mode == 3;
    const bool valid_write = write_usage == 0 || write_mode == 2 || write_mode == 3;
    if (cpu_usage == 0 || (cpu_usage & ~kCpuUsageMask) != 0 || !valid_read || !valid_write) {
        ALOGW("Rejected CPU lock usage 0x%" PRIx64 " for id=%" PRIu64, cpu_usage,
              imported->view.allocation_id);
        return AIMAPPER_ERROR_BAD_VALUE;
    }
    const bool whole =
            region.left == 0 && region.top == 0 && region.right == 0 && region.bottom == 0;
    if (!whole && (region.left < 0 || region.top < 0 || region.right <= region.left ||
                   region.bottom <= region.top ||
                   static_cast<uint32_t>(region.right) > imported->view.layout.width ||
                   static_cast<uint32_t>(region.bottom) > imported->view.layout.height)) {
        return AIMAPPER_ERROR_BAD_VALUE;
    }
    if (fence.ok() && sync_wait(fence.get(), kFenceTimeoutMs) != 0) {
        ALOGE("Acquire fence failed for id=%" PRIu64, imported->view.allocation_id);
        return AIMAPPER_ERROR_NO_RESOURCES;
    }
    std::lock_guard lock(mutex_);
    if (buffers_.count(buffer) == 0) return AIMAPPER_ERROR_BAD_BUFFER;
    if (imported->pixels == MAP_FAILED) {
        imported->pixels = mmap(nullptr, imported->view.layout.allocation_size,
                                PROT_READ | PROT_WRITE, MAP_SHARED, imported->view.pixel_fd, 0);
        if (imported->pixels == MAP_FAILED) return AIMAPPER_ERROR_NO_RESOURCES;
    }
    ++imported->lock_count;
    *data = imported->pixels;
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::unlock(buffer_handle_t buffer, int* release_fence) {
    if (release_fence == nullptr) return AIMAPPER_ERROR_BAD_VALUE;
    *release_fence = -1;
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buffer);
    if (it == buffers_.end() || it->second->lock_count == 0) return AIMAPPER_ERROR_BAD_BUFFER;
    --it->second->lock_count;
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::flushLockedBuffer(buffer_handle_t buffer) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buffer);
    if (it == buffers_.end() || it->second->lock_count == 0 || it->second->pixels == MAP_FAILED) {
        return AIMAPPER_ERROR_BAD_BUFFER;
    }
    if (msync(it->second->pixels, it->second->view.layout.allocation_size, MS_SYNC) != 0) {
        return AIMAPPER_ERROR_NO_RESOURCES;
    }
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::rereadLockedBuffer(buffer_handle_t buffer) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(buffer);
    if (it == buffers_.end() || it->second->lock_count == 0 || it->second->pixels == MAP_FAILED) {
        return AIMAPPER_ERROR_BAD_BUFFER;
    }
    if (msync(it->second->pixels, it->second->view.layout.allocation_size,
              MS_SYNC | MS_INVALIDATE) != 0) {
        return AIMAPPER_ERROR_NO_RESOURCES;
    }
    return AIMAPPER_ERROR_NONE;
}

int32_t Mapper::getMetadata(buffer_handle_t buffer, AIMapper_MetadataType type, void* data,
                            size_t size) {
    if (type.name == nullptr || strcmp(type.name, kStandardMetadataName) != 0) {
        return -AIMAPPER_ERROR_UNSUPPORTED;
    }
    return getStandardMetadata(buffer, type.value, data, size);
}

int32_t Mapper::getStandardMetadata(buffer_handle_t buffer, int64_t type, void* data, size_t size) {
    auto imported = Find(buffer);
    if (imported == nullptr) return -AIMAPPER_ERROR_BAD_BUFFER;
    return Get(*imported, static_cast<StandardMetadataType>(type), data, size);
}

int32_t Mapper::Get(const ImportedBuffer& buffer, StandardMetadataType type, void* data,
                    size_t size) {
    std::lock_guard metadata_lock(metadata_mutex_);
    MetadataFileLock file_lock(buffer.view.metadata_fd, LOCK_SH);
    if (!file_lock.locked()) return -AIMAPPER_ERROR_NO_RESOURCES;
    const fb::BufferLayout& layout = buffer.view.layout;
    auto provider = [&]<StandardMetadataType T>(auto&& provide) -> int32_t {
        if constexpr (T == StandardMetadataType::BUFFER_ID)
            return provide(buffer.view.allocation_id);
        if constexpr (T == StandardMetadataType::NAME) {
            return provide(std::string(
                    buffer.metadata->name.data(),
                    strnlen(buffer.metadata->name.data(), buffer.metadata->name.size())));
        }
        if constexpr (T == StandardMetadataType::WIDTH) return provide(uint64_t{layout.width});
        if constexpr (T == StandardMetadataType::HEIGHT) return provide(uint64_t{layout.height});
        if constexpr (T == StandardMetadataType::LAYER_COUNT) {
            return provide(uint64_t{layout.layer_count});
        }
        if constexpr (T == StandardMetadataType::PIXEL_FORMAT_REQUESTED) {
            return provide(layout.format);
        }
        if constexpr (T == StandardMetadataType::PIXEL_FORMAT_FOURCC) return provide(layout.fourcc);
        if constexpr (T == StandardMetadataType::PIXEL_FORMAT_MODIFIER) {
            return provide(uint64_t{DRM_FORMAT_MOD_LINEAR});
        }
        if constexpr (T == StandardMetadataType::USAGE) {
            return provide(static_cast<BufferUsage>(layout.usage));
        }
        if constexpr (T == StandardMetadataType::ALLOCATION_SIZE) {
            return provide(layout.allocation_size + sizeof(fb::SharedMetadata) +
                           layout.reserved_size);
        }
        if constexpr (T == StandardMetadataType::PROTECTED_CONTENT) return provide(uint64_t{0});
        if constexpr (T == StandardMetadataType::COMPRESSION) {
            return provide(android::gralloc4::Compression_None);
        }
        if constexpr (T == StandardMetadataType::INTERLACED) {
            return provide(android::gralloc4::Interlaced_None);
        }
        if constexpr (T == StandardMetadataType::CHROMA_SITING) {
            return provide(android::gralloc4::ChromaSiting_None);
        }
        if constexpr (T == StandardMetadataType::PLANE_LAYOUTS) {
            return provide(fb::GetPlaneLayouts(layout));
        }
        if constexpr (T == StandardMetadataType::CROP) {
            std::vector<Rect> crops(layout.planes.size(),
                                    {.left = 0,
                                     .top = 0,
                                     .right = static_cast<int32_t>(layout.width),
                                     .bottom = static_cast<int32_t>(layout.height)});
            return provide(crops);
        }
        if constexpr (T == StandardMetadataType::DATASPACE) {
            return provide(static_cast<Dataspace>(buffer.metadata->dataspace));
        }
        if constexpr (T == StandardMetadataType::BLEND_MODE) {
            return provide(static_cast<BlendMode>(buffer.metadata->blend_mode));
        }
        if constexpr (T == StandardMetadataType::SMPTE2086) {
            std::optional<Smpte2086> value;
            if (buffer.metadata->smpte2086_valid != 0) {
                const auto& v = buffer.metadata->smpte2086;
                value = Smpte2086{.primaryRed = {.x = v[0], .y = v[1]},
                                  .primaryGreen = {.x = v[2], .y = v[3]},
                                  .primaryBlue = {.x = v[4], .y = v[5]},
                                  .whitePoint = {.x = v[6], .y = v[7]},
                                  .maxLuminance = v[8],
                                  .minLuminance = v[9]};
            }
            return provide(value);
        }
        if constexpr (T == StandardMetadataType::CTA861_3) {
            std::optional<Cta861_3> value;
            if (buffer.metadata->cta861_3_valid != 0) {
                value = Cta861_3{.maxContentLightLevel = buffer.metadata->cta861_3[0],
                                 .maxFrameAverageLightLevel = buffer.metadata->cta861_3[1]};
            }
            return provide(value);
        }
        if constexpr (T == StandardMetadataType::STRIDE) return provide(layout.pixel_stride);
        return -AIMAPPER_ERROR_UNSUPPORTED;
    };
    return provideStandardMetadata(type, data, size, provider);
}

AIMapper_Error Mapper::setMetadata(buffer_handle_t buffer, AIMapper_MetadataType type,
                                   const void* data, size_t size) {
    if (type.name == nullptr || strcmp(type.name, kStandardMetadataName) != 0) {
        return AIMAPPER_ERROR_UNSUPPORTED;
    }
    return setStandardMetadata(buffer, type.value, data, size);
}

AIMapper_Error Mapper::setStandardMetadata(buffer_handle_t buffer, int64_t type, const void* data,
                                           size_t size) {
    auto imported = Find(buffer);
    if (imported == nullptr) return AIMAPPER_ERROR_BAD_BUFFER;
    return Set(imported.get(), static_cast<StandardMetadataType>(type), data, size);
}

AIMapper_Error Mapper::Set(ImportedBuffer* buffer, StandardMetadataType type, const void* data,
                           size_t size) {
    std::lock_guard metadata_lock(metadata_mutex_);
    MetadataFileLock file_lock(buffer->view.metadata_fd, LOCK_EX);
    if (!file_lock.locked()) return AIMAPPER_ERROR_NO_RESOURCES;
    switch (type) {
        case StandardMetadataType::BUFFER_ID:
        case StandardMetadataType::NAME:
        case StandardMetadataType::WIDTH:
        case StandardMetadataType::HEIGHT:
        case StandardMetadataType::LAYER_COUNT:
        case StandardMetadataType::PIXEL_FORMAT_REQUESTED:
        case StandardMetadataType::USAGE:
            return AIMAPPER_ERROR_BAD_VALUE;
        case StandardMetadataType::DATASPACE:
        case StandardMetadataType::BLEND_MODE:
        case StandardMetadataType::SMPTE2086:
        case StandardMetadataType::CTA861_3:
            break;
        default:
            return AIMAPPER_ERROR_UNSUPPORTED;
    }
    auto apply = [&]<StandardMetadataType T>(auto&& value) -> AIMapper_Error {
        if constexpr (T == StandardMetadataType::DATASPACE) {
            buffer->metadata->dataspace = static_cast<int32_t>(value);
            return AIMAPPER_ERROR_NONE;
        }
        if constexpr (T == StandardMetadataType::BLEND_MODE) {
            buffer->metadata->blend_mode = static_cast<int32_t>(value);
            return AIMAPPER_ERROR_NONE;
        }
        if constexpr (T == StandardMetadataType::SMPTE2086) {
            buffer->metadata->smpte2086_valid = value.has_value();
            if (value) {
                buffer->metadata->smpte2086 = {value->primaryRed.x,   value->primaryRed.y,
                                               value->primaryGreen.x, value->primaryGreen.y,
                                               value->primaryBlue.x,  value->primaryBlue.y,
                                               value->whitePoint.x,   value->whitePoint.y,
                                               value->maxLuminance,   value->minLuminance};
            }
            return AIMAPPER_ERROR_NONE;
        }
        if constexpr (T == StandardMetadataType::CTA861_3) {
            buffer->metadata->cta861_3_valid = value.has_value();
            if (value) {
                buffer->metadata->cta861_3 = {value->maxContentLightLevel,
                                              value->maxFrameAverageLightLevel};
            }
            return AIMAPPER_ERROR_NONE;
        }
        return AIMAPPER_ERROR_UNSUPPORTED;
    };
    return applyStandardMetadata(type, data, size, apply);
}

AIMapper_Error Mapper::listSupportedMetadataTypes(
        const AIMapper_MetadataTypeDescription** descriptions, size_t* count) {
    static constexpr std::array kDescriptions = {
            Describe(StandardMetadataType::BUFFER_ID, false),
            Describe(StandardMetadataType::NAME, false),
            Describe(StandardMetadataType::WIDTH, false),
            Describe(StandardMetadataType::HEIGHT, false),
            Describe(StandardMetadataType::LAYER_COUNT, false),
            Describe(StandardMetadataType::PIXEL_FORMAT_REQUESTED, false),
            Describe(StandardMetadataType::PIXEL_FORMAT_FOURCC, false),
            Describe(StandardMetadataType::PIXEL_FORMAT_MODIFIER, false),
            Describe(StandardMetadataType::USAGE, false),
            Describe(StandardMetadataType::ALLOCATION_SIZE, false),
            Describe(StandardMetadataType::PROTECTED_CONTENT, false),
            Describe(StandardMetadataType::COMPRESSION, false),
            Describe(StandardMetadataType::INTERLACED, false),
            Describe(StandardMetadataType::CHROMA_SITING, false),
            Describe(StandardMetadataType::PLANE_LAYOUTS, false),
            Describe(StandardMetadataType::CROP, false),
            Describe(StandardMetadataType::DATASPACE, true),
            Describe(StandardMetadataType::BLEND_MODE, true),
            Describe(StandardMetadataType::SMPTE2086, true),
            Describe(StandardMetadataType::CTA861_3, true),
            Describe(StandardMetadataType::STRIDE, false),
    };
    if (descriptions == nullptr || count == nullptr) return AIMAPPER_ERROR_BAD_VALUE;
    *descriptions = kDescriptions.data();
    *count = kDescriptions.size();
    return AIMAPPER_ERROR_NONE;
}

void Mapper::Dump(const std::shared_ptr<ImportedBuffer>& buffer,
                  AIMapper_DumpBufferCallback callback, void* context) {
    const AIMapper_MetadataTypeDescription* descriptions;
    size_t count;
    listSupportedMetadataTypes(&descriptions, &count);
    for (size_t i = 0; i < count; ++i) {
        const auto type = static_cast<StandardMetadataType>(descriptions[i].metadataType.value);
        const int32_t size = Get(*buffer, type, nullptr, 0);
        if (size <= 0) continue;
        std::vector<uint8_t> bytes(size);
        if (Get(*buffer, type, bytes.data(), bytes.size()) == size) {
            callback(context, descriptions[i].metadataType, bytes.data(), bytes.size());
        }
    }
}

AIMapper_Error Mapper::dumpBuffer(buffer_handle_t buffer, AIMapper_DumpBufferCallback callback,
                                  void* context) {
    auto imported = Find(buffer);
    if (imported == nullptr) return AIMAPPER_ERROR_BAD_BUFFER;
    if (callback == nullptr) return AIMAPPER_ERROR_BAD_VALUE;
    Dump(imported, callback, context);
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::dumpAllBuffers(AIMapper_BeginDumpBufferCallback begin,
                                      AIMapper_DumpBufferCallback callback, void* context) {
    if (begin == nullptr || callback == nullptr) return AIMAPPER_ERROR_BAD_VALUE;
    std::vector<std::shared_ptr<ImportedBuffer>> buffers;
    {
        std::lock_guard lock(mutex_);
        for (const auto& item : buffers_) buffers.push_back(item.second);
    }
    for (const auto& buffer : buffers) {
        begin(context);
        Dump(buffer, callback, context);
    }
    return AIMAPPER_ERROR_NONE;
}

AIMapper_Error Mapper::getReservedRegion(buffer_handle_t buffer, void** region, uint64_t* size) {
    auto imported = Find(buffer);
    if (imported == nullptr) return AIMAPPER_ERROR_BAD_BUFFER;
    if (region == nullptr || size == nullptr) return AIMAPPER_ERROR_BAD_VALUE;
    *size = imported->view.layout.reserved_size;
    *region = *size == 0
                      ? nullptr
                      : reinterpret_cast<uint8_t*>(imported->metadata) + sizeof(fb::SharedMetadata);
    return AIMAPPER_ERROR_NONE;
}

}  // namespace

extern "C" uint32_t ANDROID_HAL_MAPPER_VERSION = AIMAPPER_VERSION_5;
extern "C" uint32_t ANDROID_HAL_STABLEC_VERSION = AIMAPPER_VERSION_5;

extern "C" AIMapper_Error AIMapper_loadIMapper(AIMapper** implementation) {
    static vendor::mapper::IMapperProvider<Mapper> provider;
    return provider.load(implementation);
}
