/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-composer3"

#include "FbdevDevice.h"

#include <android-base/file.h>
#include <android-base/properties.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <log/log.h>
#include <sync/sync.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <signal.h>
#include <time.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace fb {
namespace {

constexpr int kAcquireFenceTimeoutMs = 3000;
constexpr int kVsyncInterruptSignal = SIGUSR1;

void VsyncInterruptHandler(int) {}

int64_t MonotonicNanos() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

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

bool SameModeExceptVirtualHeight(const fb_var_screeninfo& expected,
                                 const fb_var_screeninfo& actual) {
    fb_var_screeninfo normalized = actual;
    normalized.yres_virtual = expected.yres_virtual;
    normalized.activate = expected.activate;
    memcpy(normalized.reserved, expected.reserved, sizeof(normalized.reserved));
    return memcmp(&expected, &normalized, sizeof(expected)) == 0;
}

bool FramebufferSize(const fb_fix_screeninfo& fixed, const fb_var_screeninfo& info,
                     uint64_t* size) {
    const uint64_t virtual_height = std::max(info.yres_virtual, info.yres);
    return !__builtin_mul_overflow(static_cast<uint64_t>(fixed.line_length), virtual_height,
                                   size) &&
           *size != 0 && (fixed.smem_len == 0 || *size <= fixed.smem_len) && *size <= SIZE_MAX &&
           *size <= static_cast<uint64_t>(std::numeric_limits<off_t>::max());
}

bool SameBitfield(const fb_bitfield& first, const fb_bitfield& second) {
    return first.offset == second.offset && first.length == second.length &&
           first.msb_right == second.msb_right;
}

uint32_t FourccBitsPerPixel(uint32_t format) {
    switch (format) {
        case V4L2_PIX_FMT_RGB565:
            return 16;
        case V4L2_PIX_FMT_RGB24:
            return 24;
        case V4L2_PIX_FMT_XRGB32:
        case V4L2_PIX_FMT_ARGB32:
        case V4L2_PIX_FMT_XBGR32:
        case V4L2_PIX_FMT_ABGR32:
        case V4L2_PIX_FMT_ARGB2101010:
            return 32;
        default:
            return 0;
    }
}

bool EmptyBitfield(const fb_bitfield& field) {
    return field.offset == 0 && field.length == 0 && field.msb_right == 0;
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
    RestoreColorMap();
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
    const fb_fix_screeninfo original_fixed = fixed;
    const fb_var_screeninfo original_info = info;
    bool expanded_virtual_height = false;
    uint32_t double_height;
    if (info.yres != 0 && !__builtin_mul_overflow(info.yres, 2U, &double_height) &&
        info.yres_virtual < double_height) {
        fb_var_screeninfo request = info;
        request.yres_virtual = double_height;
        const fb_var_screeninfo requested_mode = request;
        if (ioctl(candidate.get(), FBIOPUT_VSCREENINFO, &request) == 0) {
            fb_fix_screeninfo returned_fixed{};
            fb_var_screeninfo returned_info{};
            uint64_t returned_size = 0;
            const bool queried =
                    ioctl(candidate.get(), FBIOGET_FSCREENINFO, &returned_fixed) == 0 &&
                    ioctl(candidate.get(), FBIOGET_VSCREENINFO, &returned_info) == 0;
            if (queried && returned_info.yres_virtual >= double_height &&
                SameModeExceptVirtualHeight(requested_mode, returned_info) &&
                FramebufferSize(returned_fixed, returned_info, &returned_size)) {
                fixed = returned_fixed;
                info = returned_info;
                expanded_virtual_height = true;
                ALOGI("Expanded %s virtual height to %u", path.c_str(), info.yres_virtual);
            } else {
                ALOGW("Driver returned unsafe geometry after virtual-height expansion on %s",
                      path.c_str());
                fb_var_screeninfo restore = original_info;
                if (ioctl(candidate.get(), FBIOPUT_VSCREENINFO, &restore) != 0 ||
                    ioctl(candidate.get(), FBIOGET_FSCREENINFO, &fixed) != 0 ||
                    ioctl(candidate.get(), FBIOGET_VSCREENINFO, &info) != 0) {
                    ALOGE("Cannot restore original fbdev mode on %s", path.c_str());
                    return false;
                }
            }
        } else {
            fixed = original_fixed;
            info = original_info;
            ALOGI("Two-page virtual framebuffer unsupported on %s: %s", path.c_str(),
                  strerror(errno));
        }
    }
    if (info.xres_virtual == 0) info.xres_virtual = info.xres;
    if (info.yres_virtual == 0) info.yres_virtual = info.yres;
    uint64_t required_line_bits;
    uint64_t required_line_bytes;
    if (__builtin_mul_overflow(static_cast<uint64_t>(info.xres),
                               static_cast<uint64_t>(info.bits_per_pixel), &required_line_bits) ||
        __builtin_add_overflow(required_line_bits, 7ULL, &required_line_bytes)) {
        ALOGE("Overflowing fbdev line geometry on %s", path.c_str());
        return false;
    }
    required_line_bytes /= 8;
    const uint32_t fourcc_bits = FourccBitsPerPixel(info.grayscale);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const bool little_endian = true;
#else
    const bool little_endian = false;
#endif
    const bool fourcc = little_endian && (fixed.capabilities & FB_CAP_FOURCC) != 0 &&
                        fixed.type == FB_TYPE_FOURCC && fixed.visual == FB_VISUAL_FOURCC &&
                        fixed.type_aux == 0 && fourcc_bits != 0 &&
                        info.bits_per_pixel == fourcc_bits && EmptyBitfield(info.red) &&
                        EmptyBitfield(info.green) && EmptyBitfield(info.blue) &&
                        EmptyBitfield(info.transp);
    const bool monochrome =
            (fixed.visual == FB_VISUAL_MONO01 || fixed.visual == FB_VISUAL_MONO10) &&
            info.bits_per_pixel == 1 && info.grayscale <= 1;
    const bool grayscale = fixed.visual == FB_VISUAL_TRUECOLOR && info.grayscale == 1 &&
                           (info.bits_per_pixel == 2 || info.bits_per_pixel == 4 ||
                            info.bits_per_pixel == 8 || info.bits_per_pixel == 16) &&
                           info.red.offset == 0 && info.red.length == info.bits_per_pixel &&
                           SameBitfield(info.red, info.green) &&
                           SameBitfield(info.red, info.blue) && info.transp.length == 0;
    const bool truecolor = fixed.visual == FB_VISUAL_TRUECOLOR && info.grayscale == 0;
    const bool directcolor = fixed.visual == FB_VISUAL_DIRECTCOLOR && info.grayscale == 0;
    const bool pseudocolor = fixed.visual == FB_VISUAL_PSEUDOCOLOR && info.bits_per_pixel == 8 &&
                             info.grayscale == 0;
    const bool static_pseudocolor = fixed.visual == FB_VISUAL_STATIC_PSEUDOCOLOR &&
                                    info.bits_per_pixel == 8 && info.grayscale == 0;
    const bool valid_nonstd =
            info.nonstd == 0 || (monochrome && info.nonstd == FB_NONSTD_REV_PIX_IN_B);
    if ((!fourcc && fixed.type != FB_TYPE_PACKED_PIXELS) ||
        (!monochrome && !grayscale && !truecolor && !directcolor && !pseudocolor &&
         !static_pseudocolor && !fourcc) ||
        !valid_nonstd || (!fourcc && info.grayscale > 1) || info.xoffset != 0 ||
        info.yoffset % std::max(info.yres, 1U) != 0 || info.xres == 0 || info.yres == 0 ||
        info.xres_virtual < info.xres || info.yres_virtual < info.yres ||
        info.yoffset > info.yres_virtual - info.yres || info.bits_per_pixel == 0 ||
        info.bits_per_pixel > 32 || info.rotate > FB_ROTATE_CCW || fixed.line_length == 0 ||
        fixed.line_length < required_line_bytes) {
        ALOGE("Unsupported fbdev geometry or channel layout on %s", path.c_str());
        return false;
    }
    if (truecolor || directcolor) {
        if (info.red.length == 0 || info.green.length == 0 || info.blue.length == 0 ||
            info.red.offset >= info.bits_per_pixel ||
            info.red.length > info.bits_per_pixel - info.red.offset ||
            info.green.offset >= info.bits_per_pixel ||
            info.green.length > info.bits_per_pixel - info.green.offset ||
            info.blue.offset >= info.bits_per_pixel ||
            info.blue.length > info.bits_per_pixel - info.blue.offset ||
            (info.transp.length != 0 &&
             (info.transp.offset >= info.bits_per_pixel ||
              info.transp.length > info.bits_per_pixel - info.transp.offset))) {
            ALOGE("Unsupported fbdev channel layout on %s", path.c_str());
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
    }
    uint64_t map_size;
    if (!FramebufferSize(fixed, info, &map_size)) {
        ALOGE("Unsafe framebuffer size on %s", path.c_str());
        return false;
    }
    void* map = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, candidate.get(), 0);
    if (map == MAP_FAILED && expanded_virtual_height) {
        ALOGW("Cannot map expanded framebuffer on %s; restoring single-page mode", path.c_str());
        fb_var_screeninfo restore = original_info;
        if (ioctl(candidate.get(), FBIOPUT_VSCREENINFO, &restore) == 0 &&
            ioctl(candidate.get(), FBIOGET_FSCREENINFO, &fixed) == 0 &&
            ioctl(candidate.get(), FBIOGET_VSCREENINFO, &info) == 0) {
            if (info.xres_virtual == 0) info.xres_virtual = info.xres;
            if (info.yres_virtual == 0) info.yres_virtual = info.yres;
            if (SameModeExceptVirtualHeight(original_info, info) &&
                fixed.type == original_fixed.type && fixed.visual == original_fixed.visual &&
                fixed.line_length == original_fixed.line_length &&
                fixed.line_length >= required_line_bytes &&
                FramebufferSize(fixed, info, &map_size)) {
                map = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, candidate.get(),
                           0);
            }
        }
    }
    if (map == MAP_FAILED) {
        ALOGE("Cannot map %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    const uint8_t mode_clear_pixel = fixed.visual == FB_VISUAL_MONO01 ? UINT8_MAX : 0;
    bool blank_supported = ioctl(candidate.get(), FBIOBLANK, FB_BLANK_POWERDOWN) == 0;
    if (!blank_supported) {
        if (errno != ENOTTY && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) {
            ALOGE("Cannot establish initial OFF state on %s: %s", path.c_str(), strerror(errno));
            munmap(map, map_size);
            return false;
        }
        ALOGW("FBIOBLANK unsupported on %s; emulating OFF with a black framebuffer", path.c_str());
        memset(map, mode_clear_pixel, map_size);
        if (msync(map, map_size, MS_SYNC) != 0) {
            ALOGW("Initial framebuffer clear failed: %s", strerror(errno));
        }
    }
    if (pseudocolor && !ConfigurePalette(candidate.get())) {
        ALOGE("Unable to install required RGB332 palette on %s", path.c_str());
        munmap(map, map_size);
        return false;
    }
    std::vector<uint16_t> saved_red;
    std::vector<uint16_t> saved_green;
    std::vector<uint16_t> saved_blue;
    std::vector<uint16_t> saved_alpha;
    if (directcolor && !ConfigureDirectColor(candidate.get(), info, &saved_red, &saved_green,
                                             &saved_blue, &saved_alpha)) {
        ALOGE("Unable to install required direct-color ramps on %s", path.c_str());
        munmap(map, map_size);
        return false;
    }
    std::vector<uint8_t> static_lookup;
    if (static_pseudocolor && !ConfigureStaticPalette(candidate.get(), &static_lookup)) {
        ALOGE("Unable to read static palette on %s", path.c_str());
        munmap(map, map_size);
        return false;
    }
    const uint8_t clear_pixel = static_lookup.empty() ? mode_clear_pixel : static_lookup[0];
    if (!blank_supported && clear_pixel != mode_clear_pixel) {
        memset(map, clear_pixel, map_size);
        if (msync(map, map_size, MS_SYNC) != 0) {
            ALOGW("Static-palette framebuffer clear failed: %s", strerror(errno));
        }
    }
    struct sigaction previous{};
    bool vsync_signal_installed = sigaction(kVsyncInterruptSignal, nullptr, &previous) == 0;
    if (vsync_signal_installed && previous.sa_handler != SIG_DFL &&
        previous.sa_handler != SIG_IGN && previous.sa_handler != VsyncInterruptHandler) {
        vsync_signal_installed = false;
        ALOGW("SIGUSR1 already has an owner; disabling hardware fbdev vsync waits");
    }
    if (vsync_signal_installed && previous.sa_handler != VsyncInterruptHandler) {
        struct sigaction action{};
        action.sa_handler = VsyncInterruptHandler;
        sigemptyset(&action.sa_mask);
        vsync_signal_installed = sigaction(kVsyncInterruptSignal, &action, nullptr) == 0;
    }
    if (!vsync_signal_installed) {
        ALOGW("Hardware fbdev vsync waits cannot be interrupted; using timed fallback");
    }
    RestoreColorMap();
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
    vsync_signal_installed_ = vsync_signal_installed;
    pseudocolor_ = pseudocolor;
    directcolor_ = directcolor;
    static_pseudocolor_ = static_pseudocolor;
    monochrome_ = monochrome;
    grayscale_ = grayscale;
    reverse_pixels_in_byte_ = (info.nonstd & FB_NONSTD_REV_PIX_IN_B) != 0;
    fourcc_ = fourcc ? info.grayscale : 0;
    clear_pixel_ = clear_pixel;
    saved_cmap_red_ = std::move(saved_red);
    saved_cmap_green_ = std::move(saved_green);
    saved_cmap_blue_ = std::move(saved_blue);
    saved_cmap_alpha_ = std::move(saved_alpha);
    static_palette_lookup_ = std::move(static_lookup);
    const std::string id(fixed_.id, strnlen(fixed_.id, sizeof(fixed_.id)));
    requires_write_flush_ = id == "efidrmdrmfb" || id == "ofdrmdrmfb" || id == "simpledrmdrmfb" ||
                            id == "vesadrmdrmfb";
    pan_supported_ = page_count_ > 1 && fixed_.ypanstep != 0 && info_.yres % fixed_.ypanstep == 0;
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

bool FbdevDevice::ConfigurePalette(int fd) const {
    std::array<uint16_t, 256> red{};
    std::array<uint16_t, 256> green{};
    std::array<uint16_t, 256> blue{};
    for (uint32_t i = 0; i < 256; ++i) {
        red[i] = static_cast<uint16_t>(((i >> 5) & 0x7) * 65535 / 7);
        green[i] = static_cast<uint16_t>(((i >> 2) & 0x7) * 65535 / 7);
        blue[i] = static_cast<uint16_t>((i & 0x3) * 65535 / 3);
    }
    fb_cmap map{.start = 0,
                .len = 256,
                .red = red.data(),
                .green = green.data(),
                .blue = blue.data(),
                .transp = nullptr};
    return ioctl(fd, FBIOPUTCMAP, &map) == 0;
}

bool FbdevDevice::ConfigureDirectColor(int fd, const fb_var_screeninfo& info,
                                       std::vector<uint16_t>* saved_red,
                                       std::vector<uint16_t>* saved_green,
                                       std::vector<uint16_t>* saved_blue,
                                       std::vector<uint16_t>* saved_alpha) const {
    const uint32_t bits =
            std::max({info.red.length, info.green.length, info.blue.length, info.transp.length});
    if (bits == 0 || bits > 16) return false;
    const uint32_t entries = 1U << bits;
    std::vector<uint16_t> original_red(entries);
    std::vector<uint16_t> original_green(entries);
    std::vector<uint16_t> original_blue(entries);
    std::vector<uint16_t> original_alpha(entries);
    fb_cmap original{.start = 0,
                     .len = entries,
                     .red = original_red.data(),
                     .green = original_green.data(),
                     .blue = original_blue.data(),
                     .transp = info.transp.length == 0 ? nullptr : original_alpha.data()};
    if (ioctl(fd, FBIOGETCMAP, &original) != 0) return false;

    auto ramp = [entries](uint32_t channel_bits) {
        std::vector<uint16_t> values(entries, UINT16_MAX);
        if (channel_bits == 0) return values;
        const uint32_t maximum = (1U << channel_bits) - 1;
        for (uint32_t i = 0; i <= maximum; ++i) {
            values[i] = static_cast<uint16_t>(static_cast<uint64_t>(i) * UINT16_MAX / maximum);
        }
        return values;
    };
    std::vector<uint16_t> red = ramp(info.red.length);
    std::vector<uint16_t> green = ramp(info.green.length);
    std::vector<uint16_t> blue = ramp(info.blue.length);
    std::vector<uint16_t> alpha = ramp(info.transp.length);
    fb_cmap linear{.start = 0,
                   .len = entries,
                   .red = red.data(),
                   .green = green.data(),
                   .blue = blue.data(),
                   .transp = info.transp.length == 0 ? nullptr : alpha.data()};
    if (ioctl(fd, FBIOPUTCMAP, &linear) != 0) {
        ioctl(fd, FBIOPUTCMAP, &original);
        return false;
    }
    *saved_red = std::move(original_red);
    *saved_green = std::move(original_green);
    *saved_blue = std::move(original_blue);
    if (info.transp.length != 0) *saved_alpha = std::move(original_alpha);
    return true;
}

void FbdevDevice::RestoreColorMap() {
    if (!fd_.ok() || saved_cmap_red_.empty()) return;
    fb_cmap original{.start = 0,
                     .len = static_cast<uint32_t>(saved_cmap_red_.size()),
                     .red = saved_cmap_red_.data(),
                     .green = saved_cmap_green_.data(),
                     .blue = saved_cmap_blue_.data(),
                     .transp = saved_cmap_alpha_.empty() ? nullptr : saved_cmap_alpha_.data()};
    if (ioctl(fd_.get(), FBIOPUTCMAP, &original) != 0) {
        ALOGW("Cannot restore direct-color map on %s: %s", path_.c_str(), strerror(errno));
    }
    saved_cmap_red_.clear();
    saved_cmap_green_.clear();
    saved_cmap_blue_.clear();
    saved_cmap_alpha_.clear();
}

bool FbdevDevice::ConfigureStaticPalette(int fd, std::vector<uint8_t>* lookup) const {
    constexpr uint32_t kEntries = 256;
    constexpr uint32_t kCubeLevels = 16;
    std::array<uint16_t, kEntries> red{};
    std::array<uint16_t, kEntries> green{};
    std::array<uint16_t, kEntries> blue{};
    fb_cmap map{.start = 0,
                .len = kEntries,
                .red = red.data(),
                .green = green.data(),
                .blue = blue.data(),
                .transp = nullptr};
    if (ioctl(fd, FBIOGETCMAP, &map) != 0) return false;
    lookup->resize(kCubeLevels * kCubeLevels * kCubeLevels);
    for (uint32_t r = 0; r < kCubeLevels; ++r) {
        for (uint32_t g = 0; g < kCubeLevels; ++g) {
            for (uint32_t b = 0; b < kCubeLevels; ++b) {
                const int32_t target_red = r * 255 / (kCubeLevels - 1);
                const int32_t target_green = g * 255 / (kCubeLevels - 1);
                const int32_t target_blue = b * 255 / (kCubeLevels - 1);
                uint32_t best_distance = UINT32_MAX;
                uint8_t best_index = 0;
                for (uint32_t i = 0; i < kEntries; ++i) {
                    const int32_t delta_red = target_red - (red[i] + 128) / 257;
                    const int32_t delta_green = target_green - (green[i] + 128) / 257;
                    const int32_t delta_blue = target_blue - (blue[i] + 128) / 257;
                    const uint32_t distance = delta_red * delta_red + delta_green * delta_green +
                                              delta_blue * delta_blue;
                    if (distance < best_distance) {
                        best_distance = distance;
                        best_index = static_cast<uint8_t>(i);
                    }
                }
                (*lookup)[(r << 8) | (g << 4) | b] = best_index;
            }
        }
    }
    return true;
}

bool FbdevDevice::Flush(uint64_t offset, uint64_t size) {
    const auto* source = static_cast<const uint8_t*>(framebuffer_) + offset;
    uint64_t written = 0;
    while (written < size) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(size - written, SSIZE_MAX));
        const ssize_t result = pwrite(fd_.get(), source + written, count, offset + written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        written += result;
    }
    if (written == size) return true;
    if (requires_write_flush_) {
        ALOGE("Incomplete fbdev damage write: %" PRIu64 " of %" PRIu64 " bytes", written, size);
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0) {
        const uint64_t aligned_offset = offset - offset % static_cast<uint64_t>(page_size);
        uint64_t end;
        if (!__builtin_add_overflow(offset, size, &end)) {
            const uint64_t sync_size = end - aligned_offset;
            if (sync_size <= framebuffer_size_ - aligned_offset &&
                msync(static_cast<uint8_t*>(framebuffer_) + aligned_offset, sync_size, MS_SYNC) ==
                        0) {
                return true;
            }
        }
    }
    ALOGW("Unable to flush fbdev mapping: %s", strerror(errno));
    return false;
}

bool FbdevDevice::SaveCurrentFrame() {
    const uint64_t page_size = static_cast<uint64_t>(info_.yres) * fixed_.line_length;
    const uint64_t offset = static_cast<uint64_t>(current_page_) * page_size;
    if (page_size > SIZE_MAX || offset > framebuffer_size_ ||
        page_size > framebuffer_size_ - offset) {
        return false;
    }
    const auto* frame = static_cast<const uint8_t*>(framebuffer_) + offset;
    last_frame_.assign(frame, frame + static_cast<size_t>(page_size));
    return true;
}

bool FbdevDevice::RestoreCurrentFrame() {
    if (last_frame_.empty()) return true;
    const uint64_t page_size = static_cast<uint64_t>(info_.yres) * fixed_.line_length;
    const uint64_t offset = static_cast<uint64_t>(current_page_) * page_size;
    if (page_size != last_frame_.size() || offset > framebuffer_size_ ||
        page_size > framebuffer_size_ - offset) {
        return false;
    }
    memcpy(static_cast<uint8_t*>(framebuffer_) + offset, last_frame_.data(), last_frame_.size());
    return Flush(offset, page_size);
}

bool FbdevDevice::Test(const std::shared_ptr<ImportedBuffer>& buffer) const {
    return buffer == nullptr || (buffer->view().layout.width == info_.xres &&
                                 buffer->view().layout.height == info_.yres &&
                                 IsScanoutFormat(buffer->view().layout.format));
}

uint32_t FbdevDevice::Pack(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) const {
    switch (fourcc_) {
        case V4L2_PIX_FMT_RGB565:
            return ((static_cast<uint32_t>(red) * 31 + 127) / 255 << 11) |
                   ((static_cast<uint32_t>(green) * 63 + 127) / 255 << 5) |
                   ((static_cast<uint32_t>(blue) * 31 + 127) / 255);
        case V4L2_PIX_FMT_RGB24:
            return red | (static_cast<uint32_t>(green) << 8) | (static_cast<uint32_t>(blue) << 16);
        case V4L2_PIX_FMT_XRGB32:
            return UINT8_MAX | (static_cast<uint32_t>(red) << 8) |
                   (static_cast<uint32_t>(green) << 16) | (static_cast<uint32_t>(blue) << 24);
        case V4L2_PIX_FMT_ARGB32:
            return alpha | (static_cast<uint32_t>(red) << 8) |
                   (static_cast<uint32_t>(green) << 16) | (static_cast<uint32_t>(blue) << 24);
        case V4L2_PIX_FMT_XBGR32:
            return blue | (static_cast<uint32_t>(green) << 8) | (static_cast<uint32_t>(red) << 16) |
                   (UINT32_C(0xff) << 24);
        case V4L2_PIX_FMT_ABGR32:
            return blue | (static_cast<uint32_t>(green) << 8) | (static_cast<uint32_t>(red) << 16) |
                   (static_cast<uint32_t>(alpha) << 24);
        case V4L2_PIX_FMT_ARGB2101010:
            return ((static_cast<uint32_t>(alpha) * 3 + 127) / 255 << 30) |
                   ((static_cast<uint32_t>(red) * 1023 + 127) / 255 << 20) |
                   ((static_cast<uint32_t>(green) * 1023 + 127) / 255 << 10) |
                   ((static_cast<uint32_t>(blue) * 1023 + 127) / 255);
        default:
            break;
    }
    if (pseudocolor_) {
        return (static_cast<uint32_t>(red) & 0xe0) | ((static_cast<uint32_t>(green) >> 3) & 0x1c) |
               (static_cast<uint32_t>(blue) >> 6);
    }
    if (static_pseudocolor_) {
        const uint32_t r = (static_cast<uint32_t>(red) * 15 + 127) / 255;
        const uint32_t g = (static_cast<uint32_t>(green) * 15 + 127) / 255;
        const uint32_t b = (static_cast<uint32_t>(blue) * 15 + 127) / 255;
        return static_palette_lookup_[(r << 8) | (g << 4) | b];
    }
    const uint32_t luminance = (77U * red + 150U * green + 29U * blue + 128) >> 8;
    if (monochrome_) {
        const bool white = luminance >= 128;
        return white == (fixed_.visual == FB_VISUAL_MONO10) ? 1 : 0;
    }
    if (grayscale_) {
        const uint32_t maximum = (1U << info_.bits_per_pixel) - 1;
        uint32_t value = (luminance * maximum + 127) / 255;
        if (info_.red.msb_right != 0) {
            uint32_t reversed = 0;
            for (uint32_t bit = 0; bit < info_.bits_per_pixel; ++bit) {
                reversed |= ((value >> bit) & 1U) << (info_.bits_per_pixel - bit - 1);
            }
            value = reversed;
        }
        return value;
    }
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
    if (bits_per_pixel < 8 && 8 % bits_per_pixel == 0 && (monochrome_ || grayscale_)) {
        const uint32_t pixels_per_byte = 8 / bits_per_pixel;
        uint32_t slot = x % pixels_per_byte;
        if (!reverse_pixels_in_byte_) slot = pixels_per_byte - slot - 1;
        const uint32_t shift = slot * bits_per_pixel;
        const uint8_t mask = static_cast<uint8_t>(((1U << bits_per_pixel) - 1) << shift);
        uint8_t& byte = row[first_bit / 8];
        byte = static_cast<uint8_t>((byte & ~mask) | ((pixel << shift) & mask));
        return;
    }
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

bool FbdevDevice::Copy(const ImportedBuffer& source, uint8_t* destination,
                       const std::vector<DamageRect>& damage) {
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
        plane.offset > layout.allocation_size || required > layout.allocation_size - plane.offset) {
        return false;
    }
    const uint8_t* pixels = source.pixels() + plane.offset;
    const uint32_t source_stride = layout.planes[0].stride;
    for (const DamageRect& rect : damage) {
        for (uint32_t y = rect.top; y < rect.bottom; ++y) {
            const uint8_t* source_row = pixels + static_cast<uint64_t>(y) * source_stride;
            uint8_t* destination_row = destination + static_cast<uint64_t>(y) * fixed_.line_length;
            for (uint32_t x = rect.left; x < rect.right; ++x) {
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
    }
    return true;
}

bool FbdevDevice::Present(const std::shared_ptr<ImportedBuffer>& buffer,
                          const std::vector<DamageRect>& damage, int acquire_fence,
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
        memset(framebuffer_, clear_pixel_, framebuffer_size_);
        if (!Flush(0, framebuffer_size_)) return false;
        if (!SaveCurrentFrame()) return false;
        if (acquire_fence >= 0) present_fence->reset(dup(acquire_fence));
        return true;
    }
    uint32_t target_page = current_page_;
    if (pan_supported_ && !damage.empty()) target_page = (current_page_ + 1) % page_count_;
    const uint64_t offset = static_cast<uint64_t>(target_page) * info_.yres * fixed_.line_length;
    const uint64_t page_size = static_cast<uint64_t>(info_.yres) * fixed_.line_length;
    if (offset + page_size > framebuffer_size_) return false;
    const std::vector<DamageRect> full_frame = {{0, 0, info_.xres, info_.yres}};
    const std::vector<DamageRect>& copy_damage = target_page == current_page_ ? damage : full_frame;
    if (!Copy(*buffer, static_cast<uint8_t*>(framebuffer_) + offset, copy_damage)) {
        ALOGE("Client target conversion failed");
        return false;
    }
    for (const DamageRect& rect : copy_damage) {
        const uint64_t first_byte = static_cast<uint64_t>(rect.left) * info_.bits_per_pixel / 8;
        const uint64_t last_byte =
                (static_cast<uint64_t>(rect.right) * info_.bits_per_pixel + 7) / 8;
        for (uint32_t y = rect.top; y < rect.bottom; ++y) {
            const uint64_t row_offset = offset + static_cast<uint64_t>(y) * fixed_.line_length;
            if (!Flush(row_offset + first_byte, last_byte - first_byte)) return false;
        }
    }
    if (pan_supported_) {
        fb_var_screeninfo pan = info_;
        pan.xoffset = 0;
        pan.yoffset = target_page * info_.yres;
        pan.activate = FB_ACTIVATE_VBL;
        if (ioctl(fd_.get(), FBIOPAN_DISPLAY, &pan) != 0) {
            ALOGW("FBIOPAN_DISPLAY failed, copying visible page: %s", strerror(errno));
            if (errno == ENOTTY || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP) {
                pan_supported_ = false;
            }
            const uint64_t visible_offset =
                    static_cast<uint64_t>(current_page_) * info_.yres * fixed_.line_length;
            if (visible_offset + page_size > framebuffer_size_ ||
                !Copy(*buffer, static_cast<uint8_t*>(framebuffer_) + visible_offset, full_frame)) {
                return false;
            }
            if (!Flush(visible_offset, page_size)) return false;
        } else {
            info_.yoffset = pan.yoffset;
            current_page_ = target_page;
        }
    }
    if (!SaveCurrentFrame()) return false;
    // A successfully waited acquire sync-file is already signaled and remains a
    // valid sync-file.
    if (acquire_fence >= 0) present_fence->reset(dup(acquire_fence));
    return true;
}

bool FbdevDevice::SetPower(bool on) {
    std::lock_guard lock(mutex_);
    if (powered_ == on) {
        if (on && HasBrightness() && backlight_power_down_ &&
            !android::base::WriteStringToFile("0", backlight_path_ + "/bl_power")) {
            ALOGE("Unable to restore backlight %s: %s", backlight_path_.c_str(), strerror(errno));
            return false;
        }
        if (on) backlight_power_down_ = false;
        return true;
    }
    if (blank_supported_ &&
        ioctl(fd_.get(), FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN) != 0) {
        ALOGE("FBIOBLANK(%d) failed: %s", on, strerror(errno));
        return false;
    }
    if (!blank_supported_ && !on) {
        memset(framebuffer_, clear_pixel_, framebuffer_size_);
        if (!Flush(0, framebuffer_size_)) return false;
    }
    if (on && !RestoreCurrentFrame()) {
        ALOGE("Unable to redraw framebuffer after unblank on %s", path_.c_str());
        return false;
    }
    if (on && HasBrightness() && !backlight_power_down_) {
        const std::string power_path = backlight_path_ + "/bl_power";
        if (access(power_path.c_str(), F_OK) == 0 &&
            !android::base::WriteStringToFile("0", power_path)) {
            ALOGE("Unable to power on backlight %s: %s", backlight_path_.c_str(), strerror(errno));
            return false;
        }
    }
    powered_ = on;
    return true;
}

void FbdevDevice::SetBacklight(std::string path, uint32_t maximum) {
    std::lock_guard lock(mutex_);
    backlight_path_ = std::move(path);
    backlight_max_ = maximum;
    backlight_power_down_ = false;
    ALOGI("Framebuffer %s uses backlight %s max=%u", path_.c_str(), backlight_path_.c_str(),
          backlight_max_);
}

bool FbdevDevice::SetBrightness(float brightness) {
    std::lock_guard lock(mutex_);
    if (!HasBrightness()) return false;
    if (brightness < 0.0F) {
        if (!android::base::WriteStringToFile("4", backlight_path_ + "/bl_power")) {
            ALOGE("Unable to power down backlight %s: %s", backlight_path_.c_str(),
                  strerror(errno));
            return false;
        }
        backlight_power_down_ = true;
        return true;
    }
    const float normalized = brightness;
    const uint32_t value = static_cast<uint32_t>(std::lround(normalized * backlight_max_));
    if (!android::base::WriteStringToFile(std::to_string(value), backlight_path_ + "/brightness")) {
        ALOGE("Unable to set backlight %s brightness=%u: %s", backlight_path_.c_str(), value,
              strerror(errno));
        return false;
    }
    const std::string power_path = backlight_path_ + "/bl_power";
    if (powered_ && !android::base::WriteStringToFile("0", power_path)) {
        ALOGE("Unable to set backlight %s power: %s", backlight_path_.c_str(), strerror(errno));
        return false;
    }
    backlight_power_down_ = false;
    return true;
}

FbdevDevice::VsyncWaitResult FbdevDevice::WaitForVsync(int64_t* timestamp_ns) {
    if (timestamp_ns == nullptr || !wait_for_vsync_supported_ || !vsync_signal_installed_) {
        return VsyncWaitResult::kFallback;
    }
#ifdef FBIO_WAITFORVSYNC
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, kVsyncInterruptSignal);
    pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
    {
        std::lock_guard lock(vsync_waiter_mutex_);
        if (vsync_interrupt_requested_) {
            vsync_interrupt_requested_ = false;
            return VsyncWaitResult::kInterrupted;
        }
        vsync_waiter_ = pthread_self();
        has_vsync_waiter_ = true;
    }
    uint32_t argument = 0;
    const int result = ioctl(fd_.get(), FBIO_WAITFORVSYNC, &argument);
    const int saved_errno = errno;
    {
        std::lock_guard lock(vsync_waiter_mutex_);
        has_vsync_waiter_ = false;
        vsync_interrupt_requested_ = false;
    }
    if (result == 0) {
        *timestamp_ns = QueryVblank(MonotonicNanos());
        return VsyncWaitResult::kHardware;
    }
    if (saved_errno == EINTR) return VsyncWaitResult::kInterrupted;
    if (saved_errno == ENOTTY || saved_errno == EINVAL || saved_errno == ENOSYS ||
        saved_errno == EOPNOTSUPP) {
        wait_for_vsync_supported_ = false;
        ALOGI("FBIO_WAITFORVSYNC unsupported on %s", path_.c_str());
    } else {
        ALOGW("FBIO_WAITFORVSYNC failed on %s: %s", path_.c_str(), strerror(saved_errno));
    }
#endif
    return VsyncWaitResult::kFallback;
}

int64_t FbdevDevice::QueryVblank(int64_t fallback_timestamp_ns) {
#ifdef FBIOGET_VBLANK
    {
        std::lock_guard lock(mutex_);
        if (!get_vblank_supported_) return fallback_timestamp_ns;
    }
    fb_vblank vblank{};
    if (ioctl(fd_.get(), FBIOGET_VBLANK, &vblank) == 0) {
        const int64_t sample_ns = MonotonicNanos();
        std::lock_guard lock(mutex_);
        const bool have_count = (vblank.flags & FB_VBLANK_HAVE_COUNT) != 0;
        if (have_count && have_vblank_count_) {
            const uint32_t elapsed = vblank.count - vblank_count_;
            if (elapsed > 1 && elapsed < UINT32_MAX / 2) missed_vblanks_ += elapsed - 1;
        }
        have_vblank_sample_ = true;
        have_vblank_count_ = have_count;
        vblank_flags_ = vblank.flags;
        if (have_count) vblank_count_ = vblank.count;
        vblank_sample_ns_ = sample_ns;
        return sample_ns;
    }
    const int saved_errno = errno;
    if (saved_errno == ENOTTY || saved_errno == EINVAL || saved_errno == ENOSYS ||
        saved_errno == EOPNOTSUPP) {
        std::lock_guard lock(mutex_);
        get_vblank_supported_ = false;
        ALOGI("FBIOGET_VBLANK unsupported on %s", path_.c_str());
    } else {
        ALOGW("FBIOGET_VBLANK failed on %s: %s", path_.c_str(), strerror(saved_errno));
    }
#endif
    return fallback_timestamp_ns;
}

void FbdevDevice::InterruptVsyncWait() {
    std::lock_guard lock(vsync_waiter_mutex_);
    vsync_interrupt_requested_ = true;
    if (has_vsync_waiter_) pthread_kill(vsync_waiter_, kVsyncInterruptSignal);
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
        << " pages=" << page_count_ << " page=" << current_page_ << " pan=" << pan_supported_
        << " c8=" << pseudocolor_ << " staticPalette=" << static_pseudocolor_
        << " directcolor=" << directcolor_ << " mono=" << monochrome_ << " grayscale=" << grayscale_
        << " fourcc=0x" << std::hex << fourcc_ << std::dec << " period=" << period_ns_
        << " dpi=" << xdpi_ << 'x' << ydpi_ << " powered=" << powered_
        << " rotation=" << info_.rotate << " savedFrame=" << !last_frame_.empty()
        << " swap_rb=" << swap_red_blue_ << " vblankSample=" << have_vblank_sample_
        << " vblankFlags=0x" << std::hex << vblank_flags_ << std::dec << " vblankCount=";
    if (have_vblank_count_) {
        out << vblank_count_;
    } else {
        out << "n/a";
    }
    out << " vblankSampleNs=" << vblank_sample_ns_ << " missedVblanks=" << missed_vblanks_ << '\n';
    return out.str();
}

}  // namespace fb
