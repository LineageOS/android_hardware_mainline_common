/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android-base/unique_fd.h>
#include <linux/fb.h>
#include <pthread.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Buffer.h"

namespace fb {

struct DamageRect {
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
};

class ImportedBuffer {
  public:
    ~ImportedBuffer();
    ImportedBuffer(const ImportedBuffer&) = delete;
    ImportedBuffer& operator=(const ImportedBuffer&) = delete;

    const HandleView& view() const { return view_; }
    const uint8_t* pixels() const { return static_cast<const uint8_t*>(pixels_); }

  private:
    friend class FbdevDevice;
    ImportedBuffer() = default;
    native_handle_t* handle_ = nullptr;
    HandleView view_;
    void* pixels_ = MAP_FAILED;
};

class FbdevDevice {
  public:
    enum class VsyncWaitResult { kHardware, kFallback, kInterrupted };

    FbdevDevice() = default;
    ~FbdevDevice();
    FbdevDevice(const FbdevDevice&) = delete;
    FbdevDevice& operator=(const FbdevDevice&) = delete;

    bool Init(const std::string& explicit_path);
    std::shared_ptr<ImportedBuffer> ImportBuffer(const native_handle_t* handle);
    bool Test(const std::shared_ptr<ImportedBuffer>& buffer) const;
    bool Present(const std::shared_ptr<ImportedBuffer>& buffer,
                 const std::vector<DamageRect>& damage, int acquire_fence,
                 android::base::unique_fd* present_fence);
    bool SetPower(bool on);
    VsyncWaitResult WaitForVsync(int64_t* timestamp_ns);
    void InterruptVsyncWait();
    std::string Dump() const;

    uint32_t width() const { return info_.xres; }
    uint32_t height() const { return info_.yres; }
    int32_t period_ns() const { return period_ns_; }
    float xdpi() const { return xdpi_; }
    float ydpi() const { return ydpi_; }
    const std::string& path() const { return path_; }

  private:
    bool InitPath(const std::string& path);
    bool Copy(const ImportedBuffer& source, uint8_t* destination,
              const std::vector<DamageRect>& damage);
    bool ConfigurePalette(int fd) const;
    bool Flush(uint64_t offset, uint64_t size);
    uint32_t Pack(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) const;
    void WritePixel(uint8_t* row, uint32_t x, uint32_t pixel) const;

    android::base::unique_fd fd_;
    std::string path_;
    fb_fix_screeninfo fixed_{};
    fb_var_screeninfo info_{};
    void* framebuffer_ = MAP_FAILED;
    size_t framebuffer_size_ = 0;
    uint32_t page_count_ = 1;
    uint32_t current_page_ = 0;
    int32_t period_ns_ = 16666666;
    float xdpi_ = 160.0F;
    float ydpi_ = 160.0F;
    bool swap_red_blue_ = false;
    bool pseudocolor_ = false;
    bool pan_supported_ = false;
    bool requires_write_flush_ = false;
    bool blank_supported_ = false;
    bool powered_ = false;
    bool wait_for_vsync_supported_ = true;
    bool vsync_signal_installed_ = false;
    pthread_t vsync_waiter_{};
    bool has_vsync_waiter_ = false;
    bool vsync_interrupt_requested_ = false;
    std::mutex vsync_waiter_mutex_;
    mutable std::mutex mutex_;
};

}  // namespace fb
