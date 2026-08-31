/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-composer3"

#include "FbdevDevice.h"

#include <android-base/properties.h>
#include <fcntl.h>
#include <log/log.h>
#include <sync/sync.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <sstream>

namespace fb {
namespace {

constexpr int kAcquireFenceTimeoutMs = 3000;

bool IsScanoutFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::RGBA_8888:
        case PixelFormat::RGBX_8888:
        case PixelFormat::BGRA_8888:
        case PixelFormat::RGB_888:
        case PixelFormat::RGB_565:
            return true;
        default:
            return false;
    }
}

uint8_t Expand5(uint32_t value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

uint8_t Expand6(uint32_t value) {
    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

uint32_t FieldMask(const fb_bitfield& field) {
    if (field.length == 0) return 0;
    const uint64_t maximum = field.length == 32 ? UINT32_MAX : (1ULL << field.length) - 1;
    return static_cast<uint32_t>(maximum << field.offset);
}

}  // namespace

ImportedBuffer::~ImportedBuffer() {
    if (pixels_ != MAP_FAILED) munmap(pixels_, view_.layout.allocation_size);
    if (handle_ != nullptr) {
        native_handle_close(handle_);
        native_handle_delete(handle_);
    }
}

FbdevDevice::~FbdevDevice() {
    if (framebuffer_ != MAP_FAILED) munmap(framebuffer_, framebuffer_size_);
}

bool FbdevDevice::Init(const std::string& explicit_path) {
    if (!explicit_path.empty()) return InitPath(explicit_path);
    for (const char* path : {"/dev/graphics/fb0", "/dev/fb0"}) {
        if (InitPath(path)) return true;
    }
    ALOGE("No usable fbdev device found");
    return false;
}

bool FbdevDevice::InitPath(const std::string& path) {
    android::base::unique_fd candidate(open(path.c_str(), O_RDWR | O_CLOEXEC));
    if (!candidate.ok()) {
        ALOGW("Cannot open %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    fb_fix_screeninfo fixed{};
    fb_var_screeninfo info{};
    if (ioctl(candidate.get(), FBIOGET_FSCREENINFO, &fixed) != 0 ||
        ioctl(candidate.get(), FBIOGET_VSCREENINFO, &info) != 0) {
        ALOGE("Cannot query %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    uint64_t required_line_bits;
    uint64_t required_line_bytes;
    if (__builtin_mul_overflow(static_cast<uint64_t>(info.xres),
                               static_cast<uint64_t>(info.bits_per_pixel), &required_line_bits) ||
        __builtin_add_overflow(required_line_bits, 7ULL, &required_line_bytes)) {
        ALOGE("Overflowing fbdev line geometry on %s", path.c_str());
        return false;
    }
    required_line_bytes /= 8;
    if (fixed.type != FB_TYPE_PACKED_PIXELS || fixed.visual != FB_VISUAL_TRUECOLOR ||
        info.nonstd != 0 || info.grayscale != 0 || info.xoffset != 0 ||
        info.yoffset % std::max(info.yres, 1U) != 0 || info.xres == 0 || info.yres == 0 ||
        info.bits_per_pixel == 0 || info.bits_per_pixel > 32 || fixed.line_length == 0 ||
        fixed.line_length < required_line_bytes || info.red.length == 0 || info.green.length == 0 ||
        info.blue.length == 0 || info.red.offset >= info.bits_per_pixel ||
        info.red.length > info.bits_per_pixel - info.red.offset ||
        info.green.offset >= info.bits_per_pixel ||
        info.green.length > info.bits_per_pixel - info.green.offset ||
        info.blue.offset >= info.bits_per_pixel ||
        info.blue.length > info.bits_per_pixel - info.blue.offset ||
        (info.transp.length != 0 &&
         (info.transp.offset >= info.bits_per_pixel ||
          info.transp.length > info.bits_per_pixel - info.transp.offset))) {
        ALOGE("Unsupported fbdev geometry or channel layout on %s", path.c_str());
        return false;
    }
    const uint32_t red_mask = FieldMask(info.red);
    const uint32_t green_mask = FieldMask(info.green);
    const uint32_t blue_mask = FieldMask(info.blue);
    const uint32_t alpha_mask = FieldMask(info.transp);
    if ((red_mask & green_mask) != 0 || (red_mask & blue_mask) != 0 ||
        (red_mask & alpha_mask) != 0 || (green_mask & blue_mask) != 0 ||
        (green_mask & alpha_mask) != 0 || (blue_mask & alpha_mask) != 0) {
        ALOGE("Overlapping fbdev channel bitfields on %s", path.c_str());
        return false;
    }
    const uint64_t virtual_height = std::max(info.yres_virtual, info.yres);
    uint64_t map_size;
    if (__builtin_mul_overflow(static_cast<uint64_t>(fixed.line_length), virtual_height,
                               &map_size) ||
        map_size == 0 || map_size > fixed.smem_len || map_size > SIZE_MAX) {
        ALOGE("Unsafe framebuffer size on %s", path.c_str());
        return false;
    }
    void* map = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, candidate.get(), 0);
    if (map == MAP_FAILED) {
        ALOGE("Cannot map %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    bool blank_supported = ioctl(candidate.get(), FBIOBLANK, FB_BLANK_POWERDOWN) == 0;
    if (!blank_supported) {
        if (errno != ENOTTY && errno != EINVAL) {
            ALOGE("Cannot establish initial OFF state on %s: %s", path.c_str(), strerror(errno));
            munmap(map, map_size);
            return false;
        }
        ALOGW("FBIOBLANK unsupported on %s; emulating OFF with a black framebuffer", path.c_str());
        memset(map, 0, map_size);
        if (msync(map, map_size, MS_SYNC) != 0) {
            ALOGW("Initial framebuffer clear failed: %s", strerror(errno));
        }
    }
    if (framebuffer_ != MAP_FAILED) munmap(framebuffer_, framebuffer_size_);
    fd_ = std::move(candidate);
    path_ = path;
    fixed_ = fixed;
    info_ = info;
    framebuffer_ = map;
    framebuffer_size_ = map_size;
    page_count_ = std::max(1U, info_.yres_virtual / info_.yres);
    current_page_ = std::min(page_count_ - 1, info_.yoffset / info_.yres);
    swap_red_blue_ = android::base::GetBoolProperty("vendor.hwc.fbdev.swap_rb", false);
    blank_supported_ = blank_supported;
    if (static_cast<int32_t>(info_.width) > 0) xdpi_ = info_.xres * 25.4F / info_.width;
    if (static_cast<int32_t>(info_.height) > 0) ydpi_ = info_.yres * 25.4F / info_.height;
    uint64_t total_x = static_cast<uint64_t>(info_.xres) + info_.left_margin + info_.right_margin +
                       info_.hsync_len;
    uint64_t total_y = static_cast<uint64_t>(info_.yres) + info_.upper_margin + info_.lower_margin +
                       info_.vsync_len;
    uint64_t picoseconds;
    if (info_.pixclock != 0 && !__builtin_mul_overflow(total_x, total_y, &picoseconds) &&
        !__builtin_mul_overflow(picoseconds, static_cast<uint64_t>(info_.pixclock), &picoseconds) &&
        picoseconds / 1000 > 0 && picoseconds / 1000 <= INT32_MAX) {
        period_ns_ = static_cast<int32_t>(picoseconds / 1000);
    }
    ALOGI("Opened %s id=%.*s %ux%u virtual=%ux%u stride=%u bpp=%u pages=%u "
          "period=%d swap_rb=%d",
          path.c_str(), static_cast<int>(sizeof(fixed_.id)), fixed_.id, info_.xres, info_.yres,
          info_.xres_virtual, info_.yres_virtual, fixed_.line_length, info_.bits_per_pixel,
          page_count_, period_ns_, swap_red_blue_);
    return true;
}

std::shared_ptr<ImportedBuffer> FbdevDevice::ImportBuffer(const native_handle_t* handle) {
    HandleView raw;
    std::string reason;
    if (!ValidateHandle(handle, &raw, &reason) || !IsScanoutFormat(raw.layout.format) ||
        raw.layout.width != info_.xres || raw.layout.height != info_.yres) {
        ALOGE("Rejected client target: %s format=%s dimensions=%ux%u display=%ux%u", reason.c_str(),
              FormatName(raw.layout.format).c_str(), raw.layout.width, raw.layout.height,
              info_.xres, info_.yres);
        return nullptr;
    }
    native_handle_t* clone = native_handle_clone(handle);
    if (clone == nullptr) return nullptr;
    auto imported = std::shared_ptr<ImportedBuffer>(new ImportedBuffer());
    imported->handle_ = clone;
    if (!ValidateHandle(clone, &imported->view_, &reason)) return nullptr;
    imported->pixels_ = mmap(nullptr, imported->view_.layout.allocation_size, PROT_READ, MAP_SHARED,
                             imported->view_.pixel_fd, 0);
    if (imported->pixels_ == MAP_FAILED) {
        ALOGE("Cannot map client target id=%" PRIu64 ": %s", imported->view_.allocation_id,
              strerror(errno));
        return nullptr;
    }
    return imported;
}

bool FbdevDevice::Test(const std::shared_ptr<ImportedBuffer>& buffer) const {
    return buffer == nullptr || (buffer->view().layout.width == info_.xres &&
                                 buffer->view().layout.height == info_.yres &&
                                 IsScanoutFormat(buffer->view().layout.format));
}

uint32_t FbdevDevice::Pack(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) const {
    auto channel = [](uint8_t value, const fb_bitfield& field) -> uint32_t {
        if (field.length == 0) return 0;
        const uint64_t maximum = field.length == 32 ? UINT32_MAX : (1ULL << field.length) - 1;
        uint32_t quantized =
                static_cast<uint32_t>((static_cast<uint64_t>(value) * maximum + 127) / 255);
        if (field.msb_right != 0) {
            uint32_t reversed = 0;
            for (uint32_t bit = 0; bit < field.length; ++bit) {
                reversed |= ((quantized >> bit) & 1U) << (field.length - bit - 1);
            }
            quantized = reversed;
        }
        return static_cast<uint32_t>(static_cast<uint64_t>(quantized) << field.offset);
    };
    return channel(red, info_.red) | channel(green, info_.green) | channel(blue, info_.blue) |
           channel(alpha, info_.transp);
}

void FbdevDevice::WritePixel(uint8_t* row, uint32_t x, uint32_t pixel) const {
    const uint32_t bits_per_pixel = info_.bits_per_pixel;
    const uint64_t first_bit = static_cast<uint64_t>(x) * bits_per_pixel;
    if ((bits_per_pixel & 7U) == 0) {
        memcpy(row + first_bit / 8, &pixel, bits_per_pixel / 8);
        return;
    }
    for (uint32_t bit = 0; bit < bits_per_pixel; ++bit) {
        const uint64_t destination_bit = first_bit + bit;
        uint8_t& byte = row[destination_bit / 8];
        const uint8_t mask = static_cast<uint8_t>(1U << (destination_bit & 7U));
        if (((pixel >> bit) & 1U) != 0) {
            byte |= mask;
        } else {
            byte &= static_cast<uint8_t>(~mask);
        }
    }
}

bool FbdevDevice::Copy(const ImportedBuffer& source, uint8_t* destination) {
    const BufferLayout& layout = source.view().layout;
    if (layout.planes.size() != 1) return false;
    uint64_t source_row_bytes;
    switch (layout.format) {
        case PixelFormat::RGBA_8888:
        case PixelFormat::RGBX_8888:
        case PixelFormat::BGRA_8888:
            source_row_bytes = static_cast<uint64_t>(layout.width) * 4;
            break;
        case PixelFormat::RGB_888:
            source_row_bytes = static_cast<uint64_t>(layout.width) * 3;
            break;
        case PixelFormat::RGB_565:
            source_row_bytes = static_cast<uint64_t>(layout.width) * 2;
            break;
        default:
            return false;
    }
    const PlaneInfo& plane = layout.planes[0];
    const uint64_t required =
            static_cast<uint64_t>(layout.height - 1) * plane.stride + source_row_bytes;
    if (plane.stride < source_row_bytes || required > plane.size ||
        plane.offset + required > layout.allocation_size) {
        return false;
    }
    const uint8_t* pixels = source.pixels() + plane.offset;
    const uint32_t source_stride = layout.planes[0].stride;
    for (uint32_t y = 0; y < info_.yres; ++y) {
        const uint8_t* source_row = pixels + static_cast<uint64_t>(y) * source_stride;
        uint8_t* destination_row = destination + static_cast<uint64_t>(y) * fixed_.line_length;
        for (uint32_t x = 0; x < info_.xres; ++x) {
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;
            uint8_t alpha = 255;
            switch (layout.format) {
                case PixelFormat::RGBA_8888:
                    red = source_row[x * 4];
                    green = source_row[x * 4 + 1];
                    blue = source_row[x * 4 + 2];
                    alpha = source_row[x * 4 + 3];
                    break;
                case PixelFormat::RGBX_8888:
                    red = source_row[x * 4];
                    green = source_row[x * 4 + 1];
                    blue = source_row[x * 4 + 2];
                    break;
                case PixelFormat::BGRA_8888:
                    blue = source_row[x * 4];
                    green = source_row[x * 4 + 1];
                    red = source_row[x * 4 + 2];
                    alpha = source_row[x * 4 + 3];
                    break;
                case PixelFormat::RGB_888:
                    red = source_row[x * 3];
                    green = source_row[x * 3 + 1];
                    blue = source_row[x * 3 + 2];
                    break;
                case PixelFormat::RGB_565: {
                    uint16_t value;
                    memcpy(&value, source_row + x * 2, sizeof(value));
                    red = Expand5((value >> 11) & 0x1f);
                    green = Expand6((value >> 5) & 0x3f);
                    blue = Expand5(value & 0x1f);
                    break;
                }
                default:
                    return false;
            }
            if (swap_red_blue_) std::swap(red, blue);
            const uint32_t output = Pack(red, green, blue, alpha);
            WritePixel(destination_row, x, output);
        }
    }
    return true;
}

bool FbdevDevice::Present(const std::shared_ptr<ImportedBuffer>& buffer, int acquire_fence,
                          android::base::unique_fd* present_fence) {
    if (present_fence == nullptr || !Test(buffer)) return false;
    present_fence->reset();
    if (acquire_fence >= 0 && sync_wait(acquire_fence, kAcquireFenceTimeoutMs) != 0) {
        ALOGE("Acquire fence timed out for id=%" PRIu64, buffer ? buffer->view().allocation_id : 0);
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!powered_) return false;
    if (buffer == nullptr) {
        memset(framebuffer_, 0, framebuffer_size_);
        if (msync(framebuffer_, framebuffer_size_, MS_SYNC) != 0) {
            ALOGW("Empty composition clear failed: %s", strerror(errno));
        }
        if (acquire_fence >= 0) present_fence->reset(dup(acquire_fence));
        return true;
    }
    uint32_t target_page = current_page_;
    if (page_count_ > 1) target_page = (current_page_ + 1) % page_count_;
    const uint64_t offset = static_cast<uint64_t>(target_page) * info_.yres * fixed_.line_length;
    const uint64_t page_size = static_cast<uint64_t>(info_.yres) * fixed_.line_length;
    if (offset + page_size > framebuffer_size_) return false;
    if (page_count_ == 1) {
        uint32_t argument = 0;
#ifdef FBIO_WAITFORVSYNC
        if (ioctl(fd_.get(), FBIO_WAITFORVSYNC, &argument) != 0 && errno != ENOTTY &&
            errno != EINVAL) {
            ALOGW("FBIO_WAITFORVSYNC failed: %s", strerror(errno));
        }
#else
        (void)argument;
#endif
    }
    if (!Copy(*buffer, static_cast<uint8_t*>(framebuffer_) + offset)) {
        ALOGE("Client target conversion failed");
        return false;
    }
    if (msync(framebuffer_, framebuffer_size_, MS_SYNC) != 0) {
        ALOGW("Framebuffer msync failed: %s", strerror(errno));
    }
    if (page_count_ > 1) {
        fb_var_screeninfo pan = info_;
        pan.xoffset = 0;
        pan.yoffset = target_page * info_.yres;
        pan.activate = FB_ACTIVATE_VBL;
        if (ioctl(fd_.get(), FBIOPAN_DISPLAY, &pan) != 0) {
            ALOGW("FBIOPAN_DISPLAY failed, copying visible page: %s", strerror(errno));
#ifdef FBIO_WAITFORVSYNC
            uint32_t argument = 0;
            if (ioctl(fd_.get(), FBIO_WAITFORVSYNC, &argument) != 0 && errno != ENOTTY &&
                errno != EINVAL) {
                ALOGW("FBIO_WAITFORVSYNC fallback failed: %s", strerror(errno));
            }
#endif
            const uint64_t visible_offset =
                    static_cast<uint64_t>(current_page_) * info_.yres * fixed_.line_length;
            if (visible_offset + page_size > framebuffer_size_ ||
                !Copy(*buffer, static_cast<uint8_t*>(framebuffer_) + visible_offset)) {
                return false;
            }
            if (msync(framebuffer_, framebuffer_size_, MS_SYNC) != 0) {
                ALOGW("Visible framebuffer msync failed: %s", strerror(errno));
            }
        } else {
            info_.yoffset = pan.yoffset;
            current_page_ = target_page;
        }
    }
    // A successfully waited acquire sync-file is already signaled and remains a
    // valid sync-file.
    if (acquire_fence >= 0) present_fence->reset(dup(acquire_fence));
    return true;
}

bool FbdevDevice::SetPower(bool on) {
    std::lock_guard lock(mutex_);
    if (powered_ == on) return true;
    if (blank_supported_ &&
        ioctl(fd_.get(), FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN) != 0) {
        ALOGE("FBIOBLANK(%d) failed: %s", on, strerror(errno));
        return false;
    }
    if (!blank_supported_ && !on) {
        memset(framebuffer_, 0, framebuffer_size_);
        if (msync(framebuffer_, framebuffer_size_, MS_SYNC) != 0) {
            ALOGW("Framebuffer blank emulation failed: %s", strerror(errno));
        }
    }
    powered_ = on;
    return true;
}

std::string FbdevDevice::Dump() const {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << " fbdev=" << path_
        << " id=" << std::string(fixed_.id, strnlen(fixed_.id, sizeof(fixed_.id)))
        << " size=" << info_.xres << 'x' << info_.yres << " virtual=" << info_.xres_virtual << 'x'
        << info_.yres_virtual << " stride=" << fixed_.line_length << " bpp=" << info_.bits_per_pixel
        << " rgba=" << info_.red.offset << ':' << info_.red.length << ',' << info_.green.offset
        << ':' << info_.green.length << ',' << info_.blue.offset << ':' << info_.blue.length
        << " pages=" << page_count_ << " page=" << current_page_ << " period=" << period_ns_
        << " dpi=" << xdpi_ << 'x' << ydpi_ << " powered=" << powered_
        << " swap_rb=" << swap_red_blue_ << '\n';
    return out.str();
}

}  // namespace fb
