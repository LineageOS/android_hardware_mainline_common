/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/graphics/allocator/BufferDescriptorInfo.h>
#include <aidl/android/hardware/graphics/common/BlendMode.h>
#include <aidl/android/hardware/graphics/common/Cta861_3.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <aidl/android/hardware/graphics/common/PlaneLayout.h>
#include <aidl/android/hardware/graphics/common/Smpte2086.h>
#include <android-base/unique_fd.h>
#include <cutils/native_handle.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fb {

using aidl::android::hardware::graphics::allocator::BufferDescriptorInfo;
using aidl::android::hardware::graphics::common::BlendMode;
using aidl::android::hardware::graphics::common::Cta861_3;
using aidl::android::hardware::graphics::common::Dataspace;
using aidl::android::hardware::graphics::common::PixelFormat;
using aidl::android::hardware::graphics::common::PlaneLayout;
using aidl::android::hardware::graphics::common::Smpte2086;

constexpr uint32_t kHandleMagic = 0x46424735;    // FBG5
constexpr uint32_t kMetadataMagic = 0x46424d35;  // FBM5
constexpr uint32_t kAbiVersion = 1;
constexpr int kHandleFds = 2;
constexpr size_t kMaxPlanes = 3;
constexpr int kHandleInts = 17 + 5 * kMaxPlanes;
constexpr uint64_t kMaxAllocationSize = 1ULL << 31;
constexpr uint64_t kMaxReservedSize = 1ULL << 24;

struct PlaneInfo {
    uint64_t offset = 0;
    uint32_t stride = 0;
    uint64_t size = 0;
};

struct BufferLayout {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layer_count = 0;
    PixelFormat format = PixelFormat::UNSPECIFIED;
    uint64_t usage = 0;
    uint32_t pixel_stride = 0;
    uint32_t fourcc = 0;
    uint64_t allocation_size = 0;
    uint64_t reserved_size = 0;
    Dataspace initial_dataspace = Dataspace::UNKNOWN;
    std::array<char, 128> name{};
    std::vector<PlaneInfo> planes;
};

// This header is followed immediately by reserved_size bytes owned by clients.
struct SharedMetadata {
    uint32_t magic;
    uint32_t abi_version;
    uint64_t allocation_id;
    uint64_t metadata_size;
    uint64_t reserved_size;
    std::array<char, 128> name;
    int32_t dataspace;
    int32_t blend_mode;
    uint32_t smpte2086_valid;
    uint32_t cta861_3_valid;
    uint32_t smpte2094_10_size;
    uint32_t smpte2094_40_size;
    std::array<float, 10> smpte2086;
    std::array<float, 2> cta861_3;
    std::array<uint8_t, 512> smpte2094_10;
    std::array<uint8_t, 512> smpte2094_40;
    std::array<uint8_t, 32> reserved;
};

struct HandleView {
    const native_handle_t* handle = nullptr;
    int pixel_fd = -1;
    int metadata_fd = -1;
    BufferLayout layout;
    uint64_t allocation_id = 0;
};

bool BuildLayout(const BufferDescriptorInfo& descriptor, BufferLayout* out, std::string* error);
bool ValidateHandle(const native_handle_t* handle, HandleView* out, std::string* error);
native_handle_t* AllocateHandle(const BufferLayout& layout, uint64_t allocation_id,
                                std::string* error);
std::vector<PlaneLayout> GetPlaneLayouts(const BufferLayout& layout);
std::string FormatName(PixelFormat format);

uint64_t JoinU64(int32_t low, int32_t high);
void SplitU64(uint64_t value, int32_t* low, int32_t* high);

}  // namespace fb
