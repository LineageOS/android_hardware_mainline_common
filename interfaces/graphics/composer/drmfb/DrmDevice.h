/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <android-base/unique_fd.h>
#include <hardware/gralloc.h>
#include <xf86drmMode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace drmfb {

struct DrmProperties {
    uint32_t connector_crtc_id = 0;
    uint32_t connector_edid = 0;
    uint32_t crtc_active = 0;
    uint32_t crtc_mode_id = 0;
    uint32_t crtc_out_fence_ptr = 0;
    uint32_t plane_fb_id = 0;
    uint32_t plane_crtc_id = 0;
    uint32_t plane_src_x = 0;
    uint32_t plane_src_y = 0;
    uint32_t plane_src_w = 0;
    uint32_t plane_src_h = 0;
    uint32_t plane_crtc_x = 0;
    uint32_t plane_crtc_y = 0;
    uint32_t plane_crtc_w = 0;
    uint32_t plane_crtc_h = 0;
    uint32_t plane_in_fence_fd = 0;
};

struct DrmConfig {
    int32_t id = 0;
    int32_t group = 0;
    drmModeModeInfo mode{};
};

struct DrmDisplay {
    int64_t id = 0;
    uint32_t connector_id = 0;
    uint32_t connector_type = 0;
    uint32_t connector_type_id = 0;
    uint32_t crtc_id = 0;
    uint32_t crtc_index = 0;
    uint32_t plane_id = 0;
    bool internal = false;
    bool connected = false;
    bool powered = false;
    bool modeset_needed = true;
    bool has_legacy_framebuffer = false;
    uint32_t legacy_format = 0;
    uint64_t legacy_modifier = 0;
    int32_t orientation_degrees = 0;
    int32_t mm_width = 0;
    int32_t mm_height = 0;
    int32_t active_config = 0;
    std::string name;
    std::vector<uint8_t> edid;
    std::vector<DrmConfig> configs;
    DrmProperties props;
};

class GemHandleRegistry {
  public:
    explicit GemHandleRegistry(int drm_fd) : drm_fd_(drm_fd) {}
    void Acquire(uint32_t handle);
    void Release(uint32_t handle);

  private:
    int drm_fd_;
    std::mutex mutex_;
    std::map<uint32_t, size_t> references_;
};

class DrmFramebuffer {
  public:
    ~DrmFramebuffer();
    DrmFramebuffer(const DrmFramebuffer&) = delete;
    DrmFramebuffer& operator=(const DrmFramebuffer&) = delete;

    uint32_t id() const { return id_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t format() const { return format_; }
    uint64_t modifier() const { return modifier_; }

  private:
    friend class DrmDevice;
    explicit DrmFramebuffer(int drm_fd) : drm_fd_(drm_fd) {}

    int drm_fd_ = -1;
    std::shared_ptr<GemHandleRegistry> registry_;
    buffer_handle_t imported_handle_ = nullptr;
    uint32_t id_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t format_ = 0;
    uint32_t source_format_ = 0;
    uint64_t modifier_ = 0;
    uint32_t source_stride_ = 0;
    uint64_t source_size_ = 0;
    size_t dumb_index_ = 0;
    size_t prepared_dumb_index_ = 0;
    bool cpu_conversion_ = false;
    std::array<uint32_t, 2> dumb_handles_{};
    std::array<uint32_t, 2> dumb_pitches_{};
    std::array<uint32_t, 2> dumb_framebuffers_{};
    std::array<uint64_t, 2> dumb_sizes_{};
    std::array<void*, 2> dumb_maps_{};
    std::array<uint32_t, 4> gem_handles_{};
};

class DrmDevice {
  public:
    struct HotplugChange {
        int64_t display = 0;
        bool connected = false;
    };

    DrmDevice() = default;
    ~DrmDevice();
    DrmDevice(const DrmDevice&) = delete;
    DrmDevice& operator=(const DrmDevice&) = delete;

    bool Init(const std::string& path);
    std::vector<HotplugChange> Rescan();
    std::map<int64_t, DrmDisplay>& displays() { return displays_; }
    const std::map<int64_t, DrmDisplay>& displays() const { return displays_; }
    int fd() const { return fd_.get(); }
    bool uses_atomic_kms() const { return atomic_kms_; }
    std::mutex& event_mutex() { return event_mutex_; }

    std::shared_ptr<DrmFramebuffer> ImportBuffer(int64_t display, buffer_handle_t handle);
    bool Test(int64_t display, const std::shared_ptr<DrmFramebuffer>& fb, int acquire_fence);
    bool TestConfiguration(int64_t display);
    bool Present(int64_t display, const std::shared_ptr<DrmFramebuffer>& fb, int acquire_fence,
                 android::base::unique_fd* out_fence);
    bool SetPower(int64_t display, bool on);
    bool SetActiveConfig(int64_t display, int32_t config);
    std::string Dump() const;

  private:
    bool InitPath(const std::string& path);
    bool DiscoverConnector(uint32_t connector_id, DrmDisplay* display);
    bool FindPipeline(drmModeConnector* connector, DrmDisplay* display,
                      const std::vector<uint32_t>& used_crtcs,
                      const std::vector<uint32_t>& used_planes);
    bool DiscoverProperties(DrmDisplay* display);
    bool AtomicCommit(DrmDisplay* display, const std::shared_ptr<DrmFramebuffer>& fb,
                      int acquire_fence, bool test_only, android::base::unique_fd* out_fence);
    bool LegacyPresent(DrmDisplay* display, const std::shared_ptr<DrmFramebuffer>& fb,
                       int acquire_fence);
    bool PrepareFramebuffer(const std::shared_ptr<DrmFramebuffer>& fb, int acquire_fence,
                            int* scanout_fence);
    bool CreateCpuConversionFramebuffer(DrmFramebuffer* fb, uint32_t format);
    bool PlaneSupportsFormat(const DrmDisplay& display, uint32_t format, uint64_t modifier) const;
    android::base::unique_fd CreateSignaledFence() const;
    uint32_t GetPropertyId(uint32_t object_id, uint32_t object_type, const char* name,
                           uint64_t* value = nullptr) const;
    bool AddProperty(drmModeAtomicReq* request, uint32_t object_id, uint32_t property_id,
                     uint64_t value) const;
    static bool IsInternal(uint32_t connector_type);
    static int32_t VsyncPeriod(const drmModeModeInfo& mode);

    android::base::unique_fd fd_;
    bool atomic_kms_ = false;
    bool modifiers_supported_ = false;
    bool syncobj_supported_ = false;
    bool swap_red_blue_ = false;
    bool cpu_conversion_enabled_ = true;
    bool firmware_kms_ = false;
    bool vboxvideo_ = false;
    int64_t next_display_id_ = 0;
    int32_t next_config_id_ = 0;
    std::map<uint32_t, int64_t> connector_display_ids_;
    std::map<std::string, int32_t> stable_config_ids_;
    std::map<int64_t, DrmDisplay> displays_;
    std::shared_ptr<GemHandleRegistry> gem_registry_;
    std::mutex event_mutex_;
};

}  // namespace drmfb
