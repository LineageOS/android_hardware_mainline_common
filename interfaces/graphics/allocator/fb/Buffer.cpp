/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-gralloc"

#include "Buffer.h"

#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidl/android/hardware/graphics/common/PlaneLayoutComponent.h>
#include <drm/drm_fourcc.h>
#include <fcntl.h>
#include <gralloctypes/Gralloc4.h>
#include <linux/memfd.h>
#include <log/log.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

namespace fb {
namespace {

using aidl::android::hardware::graphics::common::BufferUsage;
using aidl::android::hardware::graphics::common::ExtendableType;
using aidl::android::hardware::graphics::common::PlaneLayoutComponent;

constexpr uint64_t kKnownUsageMask = static_cast<uint64_t>(BufferUsage::CPU_READ_MASK) |
                                     static_cast<uint64_t>(BufferUsage::CPU_WRITE_MASK) |
                                     static_cast<uint64_t>(BufferUsage::GPU_TEXTURE) |
                                     static_cast<uint64_t>(BufferUsage::GPU_RENDER_TARGET) |
                                     static_cast<uint64_t>(BufferUsage::COMPOSER_OVERLAY) |
                                     static_cast<uint64_t>(BufferUsage::COMPOSER_CLIENT_TARGET) |
                                     static_cast<uint64_t>(BufferUsage::COMPOSER_CURSOR) |
                                     static_cast<uint64_t>(BufferUsage::VIDEO_ENCODER) |
                                     static_cast<uint64_t>(BufferUsage::CAMERA_OUTPUT) |
                                     static_cast<uint64_t>(BufferUsage::CAMERA_INPUT) |
                                     static_cast<uint64_t>(BufferUsage::RENDERSCRIPT) |
                                     static_cast<uint64_t>(BufferUsage::VIDEO_DECODER) |
                                     static_cast<uint64_t>(BufferUsage::SENSOR_DIRECT_DATA) |
                                     static_cast<uint64_t>(BufferUsage::GPU_DATA_BUFFER) |
                                     static_cast<uint64_t>(BufferUsage::GPU_CUBE_MAP) |
                                     static_cast<uint64_t>(BufferUsage::GPU_MIPMAP_COMPLETE) |
                                     static_cast<uint64_t>(BufferUsage::HW_IMAGE_ENCODER);
constexpr char kDataspaceOption[] = "android.hardware.graphics.common.Dataspace";

bool Add(uint64_t a, uint64_t b, uint64_t* out) {
    return !__builtin_add_overflow(a, b, out);
}

bool Multiply(uint64_t a, uint64_t b, uint64_t* out) {
    return !__builtin_mul_overflow(a, b, out);
}

bool Align(uint64_t value, uint64_t alignment, uint64_t* out) {
    uint64_t sum;
    return Add(value, alignment - 1, &sum) && ((*out = sum & ~(alignment - 1)), true);
}

int CreateMemfd(const char* name, uint64_t size) {
    if (size == 0 || size > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) return -1;
    int fd = static_cast<int>(syscall(SYS_memfd_create, name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

PlaneLayoutComponent Component(const ExtendableType& type, int64_t offset, int64_t size) {
    return {.type = type, .offsetInBits = offset, .sizeInBits = size};
}

PlaneLayout Plane(std::vector<PlaneLayoutComponent> components, const PlaneInfo& info,
                  int64_t increment, int64_t width, int64_t height, int64_t horizontal,
                  int64_t vertical) {
    return {.components = std::move(components),
            .offsetInBytes = static_cast<int64_t>(info.offset),
            .sampleIncrementInBits = increment,
            .strideInBytes = info.stride,
            .widthInSamples = width,
            .heightInSamples = height,
            .totalSizeInBytes = static_cast<int64_t>(info.size),
            .horizontalSubsampling = horizontal,
            .verticalSubsampling = vertical};
}

bool ExpectedHandleFormat(PixelFormat format, int plane_count, uint32_t fourcc) {
    switch (format) {
        case PixelFormat::RGBA_8888:
            return plane_count == 1 && fourcc == DRM_FORMAT_ABGR8888;
        case PixelFormat::RGBX_8888:
            return plane_count == 1 && fourcc == DRM_FORMAT_XBGR8888;
        case PixelFormat::BGRA_8888:
            return plane_count == 1 && fourcc == DRM_FORMAT_ARGB8888;
        case PixelFormat::RGB_888:
            return plane_count == 1 && fourcc == DRM_FORMAT_BGR888;
        case PixelFormat::RGB_565:
            return plane_count == 1 && fourcc == DRM_FORMAT_RGB565;
        case PixelFormat::RGBA_FP16:
            return plane_count == 1 && fourcc == DRM_FORMAT_ABGR16161616F;
        case PixelFormat::BLOB:
            return plane_count == 1 && fourcc == DRM_FORMAT_R8;
        case PixelFormat::RAW16:
            return plane_count == 1 && fourcc == DRM_FORMAT_R16;
        case PixelFormat::YV12:
            return plane_count == 3 && fourcc == DRM_FORMAT_YVU420;
        case PixelFormat::YCRCB_420_SP:
            return plane_count == 2 && fourcc == DRM_FORMAT_NV21;
        case PixelFormat::YCBCR_420_888:
            return plane_count == 3 && fourcc == DRM_FORMAT_YUV420;
        case PixelFormat::YCBCR_P010:
            return plane_count == 2 && fourcc == DRM_FORMAT_P010;
        case PixelFormat::YCBCR_P210:
            return plane_count == 2 && fourcc == DRM_FORMAT_P210;
        default:
            return false;
    }
}

}  // namespace

uint64_t JoinU64(int32_t low, int32_t high) {
    return static_cast<uint32_t>(low) | (static_cast<uint64_t>(static_cast<uint32_t>(high)) << 32);
}

void SplitU64(uint64_t value, int32_t* low, int32_t* high) {
    *low = static_cast<int32_t>(value);
    *high = static_cast<int32_t>(value >> 32);
}

std::string FormatName(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA_8888:
            return "RGBA_8888";
        case PixelFormat::RGBX_8888:
            return "RGBX_8888";
        case PixelFormat::BGRA_8888:
            return "BGRA_8888";
        case PixelFormat::RGB_888:
            return "RGB_888";
        case PixelFormat::RGB_565:
            return "RGB_565";
        case PixelFormat::RGBA_FP16:
            return "RGBA_FP16";
        case PixelFormat::BLOB:
            return "BLOB";
        case PixelFormat::RAW16:
            return "RAW16";
        case PixelFormat::YV12:
            return "YV12";
        case PixelFormat::YCRCB_420_SP:
            return "NV21";
        case PixelFormat::YCBCR_420_888:
            return "YCBCR_420_888";
        case PixelFormat::YCBCR_P010:
            return "P010";
        case PixelFormat::YCBCR_P210:
            return "P210";
        default:
            return "unsupported";
    }
}

bool BuildLayout(const BufferDescriptorInfo& descriptor, BufferLayout* out, std::string* error) {
    if (out == nullptr || descriptor.width <= 0 || descriptor.height <= 0 ||
        descriptor.layerCount != 1 || descriptor.reservedSize < 0 ||
        static_cast<uint64_t>(descriptor.reservedSize) > kMaxReservedSize) {
        if (error != nullptr) *error = "invalid dimensions, layer count, or reserved size";
        return false;
    }
    const uint64_t usage = static_cast<uint64_t>(descriptor.usage);
    const uint64_t cpu_read = usage & static_cast<uint64_t>(BufferUsage::CPU_READ_MASK);
    const uint64_t cpu_write = usage & static_cast<uint64_t>(BufferUsage::CPU_WRITE_MASK);
    if ((usage & ~kKnownUsageMask) != 0 ||
        (usage & static_cast<uint64_t>(BufferUsage::PROTECTED)) != 0 ||
        (cpu_read != static_cast<uint64_t>(BufferUsage::CPU_READ_NEVER) &&
         cpu_read != static_cast<uint64_t>(BufferUsage::CPU_READ_RARELY) &&
         cpu_read != static_cast<uint64_t>(BufferUsage::CPU_READ_OFTEN)) ||
        (cpu_write != static_cast<uint64_t>(BufferUsage::CPU_WRITE_NEVER) &&
         cpu_write != static_cast<uint64_t>(BufferUsage::CPU_WRITE_RARELY) &&
         cpu_write != static_cast<uint64_t>(BufferUsage::CPU_WRITE_OFTEN))) {
        if (error != nullptr) *error = "unknown or protected usage";
        return false;
    }
    Dataspace dataspace = Dataspace::UNKNOWN;
    bool has_dataspace = false;
    for (const auto& option : descriptor.additionalOptions) {
        if (option.name != kDataspaceOption || has_dataspace) {
            if (error != nullptr) *error = "unsupported or duplicate descriptor option";
            return false;
        }
        dataspace = static_cast<Dataspace>(option.value);
        has_dataspace = true;
    }

    BufferLayout layout;
    layout.width = descriptor.width;
    layout.height = descriptor.height;
    layout.layer_count = descriptor.layerCount;
    layout.format = descriptor.format;
    layout.usage = usage;
    layout.reserved_size = descriptor.reservedSize;
    layout.initial_dataspace = dataspace;
    memcpy(layout.name.data(), descriptor.name.data(), layout.name.size());

    uint64_t row_bytes = 0;
    uint64_t stride = 0;
    uint64_t total = 0;
    auto packed = [&](uint32_t bytes, uint32_t fourcc) {
        if (!Multiply(layout.width, bytes, &row_bytes) || !Align(row_bytes, 64, &stride) ||
            !Multiply(stride, layout.height, &total) || total > kMaxAllocationSize ||
            stride > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        layout.pixel_stride = static_cast<uint32_t>(stride / bytes);
        layout.fourcc = fourcc;
        layout.allocation_size = total;
        layout.planes = {{0, static_cast<uint32_t>(stride), total}};
        return true;
    };

    bool valid = false;
    switch (layout.format) {
        case PixelFormat::RGBA_8888:
            valid = packed(4, DRM_FORMAT_ABGR8888);
            break;
        case PixelFormat::RGBX_8888:
            valid = packed(4, DRM_FORMAT_XBGR8888);
            break;
        case PixelFormat::BGRA_8888:
            valid = packed(4, DRM_FORMAT_ARGB8888);
            break;
        case PixelFormat::RGB_888:
            valid = packed(3, DRM_FORMAT_BGR888);
            break;
        case PixelFormat::RGB_565:
            valid = packed(2, DRM_FORMAT_RGB565);
            break;
        case PixelFormat::RGBA_FP16:
            valid = packed(8, DRM_FORMAT_ABGR16161616F);
            break;
        case PixelFormat::RAW16:
            valid = (layout.width & 1) == 0 && (layout.height & 1) == 0 &&
                    packed(2, DRM_FORMAT_R16);
            break;
        case PixelFormat::BLOB:
            valid = layout.height == 1 && packed(1, DRM_FORMAT_R8);
            break;
        case PixelFormat::YV12:
        case PixelFormat::YCBCR_420_888: {
            if ((layout.width & 1) != 0 || (layout.height & 1) != 0 ||
                !Align(layout.width, 64, &stride)) {
                break;
            }
            uint64_t chroma_stride;
            uint64_t y_size;
            uint64_t c_size;
            uint64_t twice_c;
            if (!Align(stride / 2, 16, &chroma_stride) ||
                !Multiply(stride, layout.height, &y_size) ||
                !Multiply(chroma_stride, layout.height / 2, &c_size) ||
                !Multiply(c_size, 2, &twice_c) || !Add(y_size, twice_c, &total)) {
                break;
            }
            layout.pixel_stride = stride;
            layout.fourcc =
                    layout.format == PixelFormat::YV12 ? DRM_FORMAT_YVU420 : DRM_FORMAT_YUV420;
            layout.allocation_size = total;
            layout.planes.push_back({0, static_cast<uint32_t>(stride), y_size});
            if (layout.format == PixelFormat::YV12) {
                layout.planes.push_back({y_size, static_cast<uint32_t>(chroma_stride), c_size});
                layout.planes.push_back(
                        {y_size + c_size, static_cast<uint32_t>(chroma_stride), c_size});
            } else {
                layout.planes.push_back({y_size, static_cast<uint32_t>(chroma_stride), c_size});
                layout.planes.push_back(
                        {y_size + c_size, static_cast<uint32_t>(chroma_stride), c_size});
            }
            valid = total <= kMaxAllocationSize;
            break;
        }
        case PixelFormat::YCRCB_420_SP: {
            if ((layout.width & 1) != 0 || (layout.height & 1) != 0 ||
                !Align(layout.width, 64, &stride)) {
                break;
            }
            uint64_t y_size;
            uint64_t c_size;
            if (!Multiply(stride, layout.height, &y_size) ||
                !Multiply(stride, layout.height / 2, &c_size) || !Add(y_size, c_size, &total)) {
                break;
            }
            layout.pixel_stride = stride;
            layout.fourcc = DRM_FORMAT_NV21;
            layout.allocation_size = total;
            layout.planes = {{0, static_cast<uint32_t>(stride), y_size},
                             {y_size, static_cast<uint32_t>(stride), c_size}};
            valid = total <= kMaxAllocationSize;
            break;
        }
        case PixelFormat::YCBCR_P010:
        case PixelFormat::YCBCR_P210: {
            const uint32_t vertical = layout.format == PixelFormat::YCBCR_P010 ? 2 : 1;
            if ((layout.width & 1) != 0 || (layout.height % vertical) != 0 ||
                !Multiply(layout.width, 2, &row_bytes) || !Align(row_bytes, 64, &stride)) {
                break;
            }
            uint64_t y_size;
            uint64_t c_size;
            if (!Multiply(stride, layout.height, &y_size) ||
                !Multiply(stride, layout.height / vertical, &c_size) ||
                !Add(y_size, c_size, &total)) {
                break;
            }
            layout.pixel_stride = stride / 2;
            layout.fourcc =
                    layout.format == PixelFormat::YCBCR_P010 ? DRM_FORMAT_P010 : DRM_FORMAT_P210;
            layout.allocation_size = total;
            layout.planes = {{0, static_cast<uint32_t>(stride), y_size},
                             {y_size, static_cast<uint32_t>(stride), c_size}};
            valid = total <= kMaxAllocationSize;
            break;
        }
        default:
            break;
    }
    const bool transportable_stride =
            layout.pixel_stride <= static_cast<uint32_t>(INT32_MAX) &&
            std::all_of(layout.planes.begin(), layout.planes.end(), [](const PlaneInfo& plane) {
                return plane.stride <= static_cast<uint32_t>(INT32_MAX);
            });
    if (!valid || !transportable_stride) {
        if (error != nullptr) *error = "unsupported format or overflowing layout";
        return false;
    }
    *out = std::move(layout);
    return true;
}

native_handle_t* AllocateHandle(const BufferLayout& layout, uint64_t allocation_id,
                                std::string* error) {
    uint64_t metadata_size;
    if (!Add(sizeof(SharedMetadata), layout.reserved_size, &metadata_size)) return nullptr;
    android::base::unique_fd pixel(CreateMemfd("fb-gralloc-pixels", layout.allocation_size));
    android::base::unique_fd metadata(CreateMemfd("fb-gralloc-metadata", metadata_size));
    if (!pixel.ok() || !metadata.ok()) {
        if (error != nullptr) *error = std::string("memfd allocation failed: ") + strerror(errno);
        return nullptr;
    }
    void* address =
            mmap(nullptr, metadata_size, PROT_READ | PROT_WRITE, MAP_SHARED, metadata.get(), 0);
    if (address == MAP_FAILED) {
        if (error != nullptr) *error = std::string("metadata mmap failed: ") + strerror(errno);
        return nullptr;
    }
    memset(address, 0, metadata_size);
    auto* shared = static_cast<SharedMetadata*>(address);
    shared->magic = kMetadataMagic;
    shared->abi_version = kAbiVersion;
    shared->allocation_id = allocation_id;
    shared->metadata_size = metadata_size;
    shared->reserved_size = layout.reserved_size;
    shared->name = layout.name;
    shared->dataspace = static_cast<int32_t>(layout.initial_dataspace);
    shared->blend_mode = static_cast<int32_t>(BlendMode::INVALID);
    munmap(address, metadata_size);

    native_handle_t* handle = native_handle_create(kHandleFds, kHandleInts);
    if (handle == nullptr) {
        if (error != nullptr) *error = "native_handle_create failed";
        return nullptr;
    }
    handle->data[0] = pixel.release();
    handle->data[1] = metadata.release();
    int32_t* data = handle->data + kHandleFds;
    data[0] = kHandleMagic;
    data[1] = kAbiVersion;
    data[2] = layout.width;
    data[3] = layout.height;
    data[4] = layout.layer_count;
    data[5] = static_cast<int32_t>(layout.format);
    SplitU64(layout.usage, &data[6], &data[7]);
    data[8] = layout.pixel_stride;
    data[9] = layout.fourcc;
    SplitU64(layout.allocation_size, &data[10], &data[11]);
    SplitU64(allocation_id, &data[12], &data[13]);
    SplitU64(layout.reserved_size, &data[14], &data[15]);
    data[16] = layout.planes.size();
    for (size_t i = 0; i < kMaxPlanes; ++i) {
        const size_t base = 17 + i * 5;
        if (i < layout.planes.size()) {
            SplitU64(layout.planes[i].offset, &data[base], &data[base + 1]);
            data[base + 2] = layout.planes[i].stride;
            SplitU64(layout.planes[i].size, &data[base + 3], &data[base + 4]);
        } else {
            std::fill(data + base, data + base + 5, 0);
        }
    }
    return handle;
}

bool ValidateHandle(const native_handle_t* handle, HandleView* out, std::string* error) {
    if (handle == nullptr || handle->version != sizeof(native_handle_t) ||
        handle->numFds != kHandleFds || handle->numInts != kHandleInts) {
        if (error != nullptr) *error = "unexpected native handle shape";
        return false;
    }
    const int32_t* data = handle->data + kHandleFds;
    const uint64_t allocation_size = JoinU64(data[10], data[11]);
    const uint64_t reserved_size = JoinU64(data[14], data[15]);
    const int plane_count = data[16];
    if (data[0] != static_cast<int32_t>(kHandleMagic) || data[1] != kAbiVersion || data[2] <= 0 ||
        data[3] <= 0 || data[4] != 1 || data[8] <= 0 || data[9] == 0 || allocation_size == 0 ||
        allocation_size > kMaxAllocationSize || reserved_size > kMaxReservedSize ||
        plane_count <= 0 || plane_count > static_cast<int>(kMaxPlanes) || handle->data[0] < 0 ||
        handle->data[1] < 0) {
        if (error != nullptr) *error = "invalid handle fields";
        return false;
    }
    if (!ExpectedHandleFormat(static_cast<PixelFormat>(data[5]), plane_count,
                              static_cast<uint32_t>(data[9])) ||
        (static_cast<PixelFormat>(data[5]) == PixelFormat::BLOB && data[3] != 1)) {
        if (error != nullptr) *error = "inconsistent format, FourCC, or plane count";
        return false;
    }
    struct stat pixel_stat{};
    struct stat metadata_stat{};
    uint64_t required_metadata;
    if (!Add(sizeof(SharedMetadata), reserved_size, &required_metadata) ||
        fstat(handle->data[0], &pixel_stat) != 0 || fstat(handle->data[1], &metadata_stat) != 0 ||
        pixel_stat.st_size < 0 || metadata_stat.st_size < 0 ||
        static_cast<uint64_t>(pixel_stat.st_size) < allocation_size ||
        static_cast<uint64_t>(metadata_stat.st_size) < required_metadata) {
        if (error != nullptr) *error = "backing file is too small";
        return false;
    }
    constexpr int kRequiredSeals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    const int pixel_seals = fcntl(handle->data[0], F_GET_SEALS);
    const int metadata_seals = fcntl(handle->data[1], F_GET_SEALS);
    if (pixel_seals < 0 || metadata_seals < 0 || (pixel_seals & kRequiredSeals) != kRequiredSeals ||
        (metadata_seals & kRequiredSeals) != kRequiredSeals) {
        if (error != nullptr) *error = "unsealed backing file";
        return false;
    }
    HandleView view;
    view.handle = handle;
    view.pixel_fd = handle->data[0];
    view.metadata_fd = handle->data[1];
    view.layout.width = data[2];
    view.layout.height = data[3];
    view.layout.layer_count = data[4];
    view.layout.format = static_cast<PixelFormat>(data[5]);
    view.layout.usage = JoinU64(data[6], data[7]);
    view.layout.pixel_stride = data[8];
    view.layout.fourcc = static_cast<uint32_t>(data[9]);
    view.layout.allocation_size = allocation_size;
    view.allocation_id = JoinU64(data[12], data[13]);
    view.layout.reserved_size = reserved_size;
    for (int i = 0; i < plane_count; ++i) {
        const size_t base = 17 + i * 5;
        PlaneInfo plane{JoinU64(data[base], data[base + 1]), static_cast<uint32_t>(data[base + 2]),
                        JoinU64(data[base + 3], data[base + 4])};
        uint64_t end;
        if (plane.stride == 0 || plane.size == 0 || !Add(plane.offset, plane.size, &end) ||
            end > allocation_size) {
            if (error != nullptr) *error = "invalid plane bounds";
            return false;
        }
        view.layout.planes.push_back(plane);
    }
    BufferDescriptorInfo descriptor;
    descriptor.width = view.layout.width;
    descriptor.height = view.layout.height;
    descriptor.layerCount = view.layout.layer_count;
    descriptor.format = view.layout.format;
    descriptor.usage = static_cast<BufferUsage>(view.layout.usage);
    descriptor.reservedSize = view.layout.reserved_size;
    BufferLayout expected;
    if (!BuildLayout(descriptor, &expected, nullptr) ||
        expected.pixel_stride != view.layout.pixel_stride ||
        expected.fourcc != view.layout.fourcc ||
        expected.allocation_size != view.layout.allocation_size ||
        expected.planes.size() != view.layout.planes.size()) {
        if (error != nullptr) *error = "inconsistent transported layout";
        return false;
    }
    for (size_t i = 0; i < expected.planes.size(); ++i) {
        if (expected.planes[i].offset != view.layout.planes[i].offset ||
            expected.planes[i].stride != view.layout.planes[i].stride ||
            expected.planes[i].size != view.layout.planes[i].size) {
            if (error != nullptr) *error = "inconsistent transported plane";
            return false;
        }
    }
    *out = std::move(view);
    return true;
}

std::vector<PlaneLayout> GetPlaneLayouts(const BufferLayout& layout) {
    using namespace android::gralloc4;
    const int64_t width = layout.width;
    const int64_t height = layout.height;
    const auto& p = layout.planes;
    switch (layout.format) {
        case PixelFormat::RGBA_8888:
            return {Plane({Component(PlaneLayoutComponentType_R, 0, 8),
                           Component(PlaneLayoutComponentType_G, 8, 8),
                           Component(PlaneLayoutComponentType_B, 16, 8),
                           Component(PlaneLayoutComponentType_A, 24, 8)},
                          p[0], 32, width, height, 1, 1)};
        case PixelFormat::RGBX_8888:
            return {Plane({Component(PlaneLayoutComponentType_R, 0, 8),
                           Component(PlaneLayoutComponentType_G, 8, 8),
                           Component(PlaneLayoutComponentType_B, 16, 8)},
                          p[0], 32, width, height, 1, 1)};
        case PixelFormat::BGRA_8888:
            return {Plane({Component(PlaneLayoutComponentType_B, 0, 8),
                           Component(PlaneLayoutComponentType_G, 8, 8),
                           Component(PlaneLayoutComponentType_R, 16, 8),
                           Component(PlaneLayoutComponentType_A, 24, 8)},
                          p[0], 32, width, height, 1, 1)};
        case PixelFormat::RGB_888:
            return {Plane({Component(PlaneLayoutComponentType_R, 0, 8),
                           Component(PlaneLayoutComponentType_G, 8, 8),
                           Component(PlaneLayoutComponentType_B, 16, 8)},
                          p[0], 24, width, height, 1, 1)};
        case PixelFormat::RGB_565:
            return {Plane({Component(PlaneLayoutComponentType_R, 11, 5),
                           Component(PlaneLayoutComponentType_G, 5, 6),
                           Component(PlaneLayoutComponentType_B, 0, 5)},
                          p[0], 16, width, height, 1, 1)};
        case PixelFormat::RGBA_FP16:
            return {Plane({Component(PlaneLayoutComponentType_R, 0, 16),
                           Component(PlaneLayoutComponentType_G, 16, 16),
                           Component(PlaneLayoutComponentType_B, 32, 16),
                           Component(PlaneLayoutComponentType_A, 48, 16)},
                          p[0], 64, width, height, 1, 1)};
        case PixelFormat::RAW16:
            return {Plane({Component(PlaneLayoutComponentType_RAW, 0, 16)}, p[0], 16, width, height,
                          1, 1)};
        case PixelFormat::BLOB:
            return {Plane({}, p[0], 8, width, height, 1, 1)};
        case PixelFormat::YV12:
            return {Plane({Component(PlaneLayoutComponentType_Y, 0, 8)}, p[0], 8, width, height, 1,
                          1),
                    Plane({Component(PlaneLayoutComponentType_CR, 0, 8)}, p[1], 8, width / 2,
                          height / 2, 2, 2),
                    Plane({Component(PlaneLayoutComponentType_CB, 0, 8)}, p[2], 8, width / 2,
                          height / 2, 2, 2)};
        case PixelFormat::YCBCR_420_888:
            return {Plane({Component(PlaneLayoutComponentType_Y, 0, 8)}, p[0], 8, width, height, 1,
                          1),
                    Plane({Component(PlaneLayoutComponentType_CB, 0, 8)}, p[1], 8, width / 2,
                          height / 2, 2, 2),
                    Plane({Component(PlaneLayoutComponentType_CR, 0, 8)}, p[2], 8, width / 2,
                          height / 2, 2, 2)};
        case PixelFormat::YCRCB_420_SP:
            return {Plane({Component(PlaneLayoutComponentType_Y, 0, 8)}, p[0], 8, width, height, 1,
                          1),
                    Plane({Component(PlaneLayoutComponentType_CR, 0, 8),
                           Component(PlaneLayoutComponentType_CB, 8, 8)},
                          p[1], 16, width / 2, height / 2, 2, 2)};
        case PixelFormat::YCBCR_P010:
        case PixelFormat::YCBCR_P210: {
            const int64_t vertical = layout.format == PixelFormat::YCBCR_P010 ? 2 : 1;
            return {Plane({Component(PlaneLayoutComponentType_Y, 6, 10)}, p[0], 16, width, height,
                          1, 1),
                    Plane({Component(PlaneLayoutComponentType_CB, 6, 10),
                           Component(PlaneLayoutComponentType_CR, 22, 10)},
                          p[1], 32, width / 2, height / vertical, 2, vertical)};
        }
        default:
            return {};
    }
}

}  // namespace fb
