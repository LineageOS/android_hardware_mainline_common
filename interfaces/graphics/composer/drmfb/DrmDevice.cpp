/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "drmfb-composer3"

#include "DrmDevice.h"

#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <log/log.h>
#include <poll.h>
#include <sync/sync.h>
#include <sys/mman.h>
#include <ui/GraphicBufferMapper.h>
#include <xf86drm.h>

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>

namespace drmfb {
namespace {

using android::base::StringPrintf;
constexpr size_t kMaxPlanes = 4;

uint32_t SwapRedBlueFormat(uint32_t format) {
    switch (format) {
        case DRM_FORMAT_XRGB8888:
            return DRM_FORMAT_XBGR8888;
        case DRM_FORMAT_XBGR8888:
            return DRM_FORMAT_XRGB8888;
        case DRM_FORMAT_ARGB8888:
            return DRM_FORMAT_ABGR8888;
        case DRM_FORMAT_ABGR8888:
            return DRM_FORMAT_ARGB8888;
        default:
            return 0;
    }
}

struct CardCandidate {
    std::string path;
    int score = 0;
};

bool IsInternalConnector(uint32_t type) {
    return type == DRM_MODE_CONNECTOR_LVDS || type == DRM_MODE_CONNECTOR_eDP ||
           type == DRM_MODE_CONNECTOR_DSI || type == DRM_MODE_CONNECTOR_DPI ||
           type == DRM_MODE_CONNECTOR_VIRTUAL;
}

int ScoreCard(const std::string& path) {
    android::base::unique_fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
    if (!fd.ok()) return -1;
    drmModeRes* resources = drmModeGetResources(fd.get());
    if (resources == nullptr || resources->count_crtcs == 0 || resources->count_connectors == 0 ||
        resources->count_encoders == 0) {
        if (resources != nullptr) drmModeFreeResources(resources);
        return -1;
    }
    const bool probe_connectors = drmSetMaster(fd.get()) == 0;
    bool has_internal = false;
    int connected_count = 0;
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnector* connector =
                probe_connectors ? drmModeGetConnector(fd.get(), resources->connectors[i])
                                 : drmModeGetConnectorCurrent(fd.get(), resources->connectors[i]);
        if (connector == nullptr) continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
            ++connected_count;
            has_internal |= IsInternalConnector(connector->connector_type);
        }
        drmModeFreeConnector(connector);
    }
    if (probe_connectors) drmDropMaster(fd.get());
    drmModeFreeResources(resources);
    constexpr int kInternalDisplayScore = 10000;
    constexpr int kExternalDisplayScore = 5000;
    constexpr int kHeadlessScore = 100;
    return (has_internal          ? kInternalDisplayScore
            : connected_count > 0 ? kExternalDisplayScore
                                  : kHeadlessScore) +
           connected_count;
}

int32_t GetPanelOrientation(int fd, uint32_t connector_id) {
    drmModeObjectProperties* properties =
            drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (properties == nullptr) return 0;
    int32_t orientation = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i) {
        drmModePropertyRes* property = drmModeGetProperty(fd, properties->props[i]);
        if (property == nullptr) continue;
        if (strcmp(property->name, "panel orientation") == 0) {
            const uint64_t value = properties->prop_values[i];
            for (int j = 0; j < property->count_enums; ++j) {
                if (property->enums[j].value != value) continue;
                if (strcmp(property->enums[j].name, "Upside Down") == 0) {
                    orientation = 180;
                } else if (strcmp(property->enums[j].name, "Left Side Up") == 0) {
                    orientation = 270;
                } else if (strcmp(property->enums[j].name, "Right Side Up") == 0) {
                    orientation = 90;
                }
                break;
            }
        }
        drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(properties);
    return orientation;
}

std::string ModeKey(uint32_t connector_id, const drmModeModeInfo& mode) {
    std::ostringstream key;
    key << connector_id << ':' << mode.clock << ':' << mode.hdisplay << ':' << mode.hsync_start
        << ':' << mode.hsync_end << ':' << mode.htotal << ':' << mode.hskew << ':' << mode.vdisplay
        << ':' << mode.vsync_start << ':' << mode.vsync_end << ':' << mode.vtotal << ':'
        << mode.vscan << ':' << mode.flags << ':' << mode.vrefresh;
    return key.str();
}

bool IsUsableEdid(const std::vector<uint8_t>& edid) {
    static constexpr uint8_t kHeader[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    if (edid.size() < 128 || edid.size() % 128 != 0 ||
        !std::equal(std::begin(kHeader), std::end(kHeader), edid.begin())) {
        return false;
    }
    for (size_t block = 0; block < edid.size(); block += 128) {
        uint8_t sum = 0;
        for (size_t i = block; i < block + 128; ++i) sum += edid[i];
        if (sum != 0) return false;
    }
    return true;
}

struct PageFlipResult {
    bool complete = false;
};

void HandlePageFlip(int, unsigned int, unsigned int, unsigned int, unsigned int, void* data) {
    static_cast<PageFlipResult*>(data)->complete = true;
}

size_t CountConnectedConnectors(int fd) {
    drmModeRes* resources = drmModeGetResources(fd);
    if (resources == nullptr) return 0;
    size_t count = 0;
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (connector != nullptr && connector->connection == DRM_MODE_CONNECTED &&
            connector->count_modes > 0) {
            ++count;
        }
        if (connector != nullptr) drmModeFreeConnector(connector);
    }
    drmModeFreeResources(resources);
    return count;
}

}  // namespace

DrmFramebuffer::~DrmFramebuffer() {
    if (!cpu_swap_red_blue_ && id_ != 0 && drmModeRmFB(drm_fd_, id_) != 0) {
        ALOGW("Failed to remove framebuffer %u: %s", id_, strerror(errno));
    }
    for (size_t i = 0; i < dumb_handles_.size(); ++i) {
        if (dumb_framebuffers_[i] != 0 && drmModeRmFB(drm_fd_, dumb_framebuffers_[i]) != 0) {
            ALOGW("Failed to remove dumb framebuffer %u: %s", dumb_framebuffers_[i],
                  strerror(errno));
        }
        if (dumb_maps_[i] != nullptr && dumb_maps_[i] != MAP_FAILED) {
            munmap(dumb_maps_[i], dumb_sizes_[i]);
        }
        if (dumb_handles_[i] != 0 && drmModeDestroyDumbBuffer(drm_fd_, dumb_handles_[i]) != 0) {
            ALOGW("Failed to destroy dumb buffer %u: %s", dumb_handles_[i], strerror(errno));
        }
    }
    if (imported_handle_ != nullptr) {
        android::GraphicBufferMapper::get().freeBuffer(imported_handle_);
    }
    if (registry_ != nullptr) {
        std::set<uint32_t> released;
        for (uint32_t handle : gem_handles_) {
            if (handle != 0 && released.insert(handle).second) {
                registry_->Release(handle);
            }
        }
    }
}

void GemHandleRegistry::Acquire(uint32_t handle) {
    std::lock_guard lock(mutex_);
    ++references_[handle];
}

void GemHandleRegistry::Release(uint32_t handle) {
    std::lock_guard lock(mutex_);
    auto it = references_.find(handle);
    if (it == references_.end()) return;
    if (--it->second != 0) return;
    references_.erase(it);
    struct drm_gem_close close_arg{};
    close_arg.handle = handle;
    if (drmIoctl(drm_fd_, DRM_IOCTL_GEM_CLOSE, &close_arg) != 0) {
        ALOGW("Failed to close GEM handle %u: %s", handle, strerror(errno));
    }
}

DrmDevice::~DrmDevice() {
    displays_.clear();
    gem_registry_.reset();
    if (fd_.ok()) drmDropMaster(fd_.get());
}

bool DrmDevice::Init(const std::string& path) {
    if (!path.empty()) return InitPath(path);

    const int max_devices = drmGetDevices2(0, nullptr, 0);
    if (max_devices <= 0) {
        ALOGE("No DRM devices found: %s", max_devices < 0 ? strerror(-max_devices) : "none");
        return false;
    }
    std::vector<drmDevicePtr> devices(max_devices, nullptr);
    const int device_count = drmGetDevices2(0, devices.data(), max_devices);
    if (device_count <= 0) {
        ALOGE("Unable to enumerate DRM devices: %s",
              device_count < 0 ? strerror(-device_count) : "none");
        return false;
    }
    std::vector<CardCandidate> candidates;
    std::set<std::string> paths;
    for (int i = 0; i < device_count; ++i) {
        drmDevicePtr device = devices[i];
        if (device == nullptr || (device->available_nodes & (1 << DRM_NODE_PRIMARY)) == 0 ||
            device->nodes[DRM_NODE_PRIMARY] == nullptr) {
            continue;
        }
        std::string candidate_path = device->nodes[DRM_NODE_PRIMARY];
        if (!paths.insert(candidate_path).second) continue;
        const int score = ScoreCard(candidate_path);
        if (score >= 0) candidates.push_back({std::move(candidate_path), score});
    }
    drmFreeDevices(devices.data(), device_count);
    std::sort(candidates.begin(), candidates.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.score != rhs.score ? lhs.score > rhs.score : lhs.path < rhs.path;
    });
    for (const CardCandidate& candidate : candidates) {
        ALOGI("Trying auto-detected DRM device %s (score=%d)", candidate.path.c_str(),
              candidate.score);
        if (InitPath(candidate.path)) return true;
    }
    ALOGE("No usable DRM KMS primary node found");
    return false;
}

bool DrmDevice::InitPath(const std::string& path) {
    displays_.clear();
    connector_display_ids_.clear();
    stable_config_ids_.clear();
    gem_registry_.reset();
    if (fd_.ok() && drmIsMaster(fd_.get()) != 0) drmDropMaster(fd_.get());
    fd_.reset();
    atomic_kms_ = false;
    modifiers_supported_ = false;
    syncobj_supported_ = false;
    swap_red_blue_ = ::android::base::GetBoolProperty("vendor.hwc.drmfb.swap_rb", false);
    next_display_id_ = 0;
    next_config_id_ = 0;
    fd_.reset(open(path.c_str(), O_RDWR | O_CLOEXEC));
    if (!fd_.ok()) {
        ALOGE("Cannot open DRM device %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    gem_registry_ = std::make_shared<GemHandleRegistry>(fd_.get());
    const bool universal_planes =
            drmSetClientCap(fd_.get(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0;
    atomic_kms_ = universal_planes && drmSetClientCap(fd_.get(), DRM_CLIENT_CAP_ATOMIC, 1) == 0;
    if (!atomic_kms_) {
        ALOGW("Atomic KMS unavailable; using legacy modesetting");
    }
    if (drmSetMaster(fd_.get()) != 0 || drmIsMaster(fd_.get()) == 0) {
        ALOGE("DRM master access is required: %s", strerror(errno));
        return false;
    }
    uint64_t modifiers = 0;
    modifiers_supported_ =
            drmGetCap(fd_.get(), DRM_CAP_ADDFB2_MODIFIERS, &modifiers) == 0 && modifiers != 0;
    uint64_t syncobj = 0;
    syncobj_supported_ = drmGetCap(fd_.get(), DRM_CAP_SYNCOBJ, &syncobj) == 0 && syncobj != 0;
    const size_t connected_connectors = CountConnectedConnectors(fd_.get());
    Rescan();
    const size_t discovered_displays =
            std::count_if(displays_.begin(), displays_.end(),
                          [](const auto& item) { return item.second.connected; });
    if (atomic_kms_ && discovered_displays < connected_connectors) {
        ALOGW("Atomic discovery found %zu of %zu connected displays; retrying with legacy KMS",
              discovered_displays, connected_connectors);
        atomic_kms_ = false;
        displays_.clear();
        Rescan();
    }
    const bool has_connected_display =
            std::any_of(displays_.begin(), displays_.end(),
                        [](const auto& item) { return item.second.connected; });
    if (connected_connectors > 0 && !has_connected_display) {
        ALOGE("No usable pipeline found on %s", path.c_str());
        return false;
    }
    ALOGI("Opened %s with %zu connector records; backend=%s modifiers=%d swap_rb=%d", path.c_str(),
          displays_.size(), atomic_kms_ ? "atomic" : "legacy", modifiers_supported_,
          swap_red_blue_);
    return true;
}

bool DrmDevice::IsInternal(uint32_t type) {
    return IsInternalConnector(type);
}

int32_t DrmDevice::VsyncPeriod(const drmModeModeInfo& mode) {
    uint64_t total = static_cast<uint64_t>(mode.htotal) * mode.vtotal;
    if ((mode.flags & DRM_MODE_FLAG_INTERLACE) != 0) total /= 2;
    if ((mode.flags & DRM_MODE_FLAG_DBLSCAN) != 0) total *= 2;
    total *= std::max<uint16_t>(mode.vscan, 1);
    return mode.clock == 0 ? 16666666 : static_cast<int32_t>(total * 1000000ULL / mode.clock);
}

std::vector<DrmDevice::HotplugChange> DrmDevice::Rescan() {
    std::vector<HotplugChange> changes;
    drmModeRes* resources = drmModeGetResources(fd_.get());
    if (resources == nullptr) {
        ALOGE("drmModeGetResources failed: %s", strerror(errno));
        return changes;
    }
    std::vector<uint32_t> connectors(resources->connectors,
                                     resources->connectors + resources->count_connectors);
    drmModeFreeResources(resources);
    std::sort(connectors.begin(), connectors.end(), [this](uint32_t a, uint32_t b) {
        drmModeConnector* ca = drmModeGetConnector(fd_.get(), a);
        drmModeConnector* cb = drmModeGetConnector(fd_.get(), b);
        const bool ai = ca != nullptr && IsInternal(ca->connector_type);
        const bool bi = cb != nullptr && IsInternal(cb->connector_type);
        if (ca != nullptr) drmModeFreeConnector(ca);
        if (cb != nullptr) drmModeFreeConnector(cb);
        return ai != bi ? ai : a < b;
    });

    std::map<int64_t, bool> previous_connections;
    for (auto& [id, display] : displays_) {
        previous_connections[id] = display.connected;
        display.connected = false;
    }
    std::vector<uint32_t> used_crtcs;
    std::vector<uint32_t> used_planes;
    std::set<int64_t> seen_displays;
    for (uint32_t connector_id : connectors) {
        int64_t display_id;
        auto known = connector_display_ids_.find(connector_id);
        if (known == connector_display_ids_.end()) {
            display_id = next_display_id_++;
            connector_display_ids_[connector_id] = display_id;
        } else {
            display_id = known->second;
        }
        seen_displays.insert(display_id);
        const bool was_connected = previous_connections[display_id];
        DrmDisplay candidate;
        candidate.id = display_id;
        if (!DiscoverConnector(connector_id, &candidate) || !candidate.connected) {
            if (was_connected) changes.push_back({display_id, false});
            continue;
        }
        if (!FindPipeline(drmModeGetConnector(fd_.get(), connector_id), &candidate, used_crtcs,
                          used_planes) ||
            !DiscoverProperties(&candidate)) {
            ALOGW("No complete %s pipeline for connector %u", atomic_kms_ ? "atomic" : "legacy",
                  connector_id);
            if (was_connected) changes.push_back({display_id, false});
            continue;
        }
        used_crtcs.push_back(candidate.crtc_id);
        used_planes.push_back(candidate.plane_id);
        auto old = displays_.find(display_id);
        if (old != displays_.end() && was_connected) {
            const int32_t discovered_config = candidate.active_config;
            candidate.powered = old->second.powered;
            candidate.modeset_needed = old->second.modeset_needed ||
                                       candidate.crtc_id != old->second.crtc_id ||
                                       candidate.plane_id != old->second.plane_id ||
                                       discovered_config != old->second.active_config;
            candidate.has_legacy_framebuffer = old->second.has_legacy_framebuffer;
            candidate.legacy_format = old->second.legacy_format;
            candidate.legacy_modifier = old->second.legacy_modifier;
            bool preserved_config = false;
            for (const DrmConfig& config : candidate.configs) {
                if (config.id == old->second.active_config) {
                    candidate.active_config = config.id;
                    preserved_config = true;
                    break;
                }
            }
            if (!preserved_config) candidate.modeset_needed = true;
        }
        displays_[display_id] = std::move(candidate);
        if (!was_connected) changes.push_back({display_id, true});
    }
    for (const auto& [id, was_connected] : previous_connections) {
        if (was_connected && seen_displays.count(id) == 0) {
            changes.push_back({id, false});
        }
    }
    return changes;
}

bool DrmDevice::DiscoverConnector(uint32_t connector_id, DrmDisplay* display) {
    drmModeConnector* connector = drmModeGetConnector(fd_.get(), connector_id);
    if (connector == nullptr) return false;
    display->connector_id = connector_id;
    display->connector_type = connector->connector_type;
    display->connector_type_id = connector->connector_type_id;
    display->internal = IsInternal(connector->connector_type);
    display->connected = connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0;
    display->mm_width = connector->mmWidth;
    display->mm_height = connector->mmHeight;
    display->name = StringPrintf("%s-%u", drmModeGetConnectorTypeName(connector->connector_type),
                                 connector->connector_type_id);
    if (display->connected) {
        for (int i = 0; i < connector->count_modes; ++i) {
            DrmConfig config;
            config.mode = connector->modes[i];
            const bool duplicate =
                    std::any_of(display->configs.begin(), display->configs.end(),
                                [&config](const DrmConfig& existing) {
                                    return existing.mode.hdisplay == config.mode.hdisplay &&
                                           existing.mode.vdisplay == config.mode.vdisplay &&
                                           VsyncPeriod(existing.mode) == VsyncPeriod(config.mode);
                                });
            if (duplicate) continue;
            const std::string key = ModeKey(connector_id, config.mode);
            auto [it, inserted] = stable_config_ids_.emplace(key, next_config_id_);
            if (inserted) ++next_config_id_;
            config.id = it->second;
            config.group =
                    (static_cast<int32_t>(config.mode.hdisplay) << 16) | config.mode.vdisplay;
            display->configs.push_back(config);
            if (i == 0 || (config.mode.type & DRM_MODE_TYPE_PREFERRED) != 0) {
                display->active_config = config.id;
            }
        }
    }
    drmModeFreeConnector(connector);
    return true;
}

bool DrmDevice::FindPipeline(drmModeConnector* connector, DrmDisplay* display,
                             const std::vector<uint32_t>& used_crtcs,
                             const std::vector<uint32_t>& used_planes) {
    if (connector == nullptr) return false;
    drmModeRes* resources = drmModeGetResources(fd_.get());
    drmModePlaneRes* planes = atomic_kms_ ? drmModeGetPlaneResources(fd_.get()) : nullptr;
    if (resources == nullptr || (atomic_kms_ && planes == nullptr)) {
        if (resources != nullptr) drmModeFreeResources(resources);
        if (planes != nullptr) drmModeFreePlaneResources(planes);
        drmModeFreeConnector(connector);
        return false;
    }
    bool found = false;
    for (int e = 0; e < connector->count_encoders && !found; ++e) {
        drmModeEncoder* encoder = drmModeGetEncoder(fd_.get(), connector->encoders[e]);
        if (encoder == nullptr) continue;
        for (int c = 0; c < resources->count_crtcs && !found; ++c) {
            const uint32_t crtc = resources->crtcs[c];
            if ((encoder->possible_crtcs & (1U << c)) == 0 ||
                std::find(used_crtcs.begin(), used_crtcs.end(), crtc) != used_crtcs.end())
                continue;
            if (!atomic_kms_) {
                display->crtc_id = crtc;
                display->crtc_index = c;
                found = true;
                break;
            }
            for (uint32_t p = 0; p < planes->count_planes; ++p) {
                drmModePlane* plane = drmModeGetPlane(fd_.get(), planes->planes[p]);
                if (plane == nullptr) continue;
                uint64_t type = 0;
                GetPropertyId(plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
                const bool usable = type == DRM_PLANE_TYPE_PRIMARY &&
                                    (plane->possible_crtcs & (1U << c)) != 0 &&
                                    std::find(used_planes.begin(), used_planes.end(),
                                              plane->plane_id) == used_planes.end();
                if (usable) {
                    display->crtc_id = crtc;
                    display->crtc_index = c;
                    display->plane_id = plane->plane_id;
                    found = true;
                }
                drmModeFreePlane(plane);
                if (found) break;
            }
        }
        drmModeFreeEncoder(encoder);
    }
    if (planes != nullptr) drmModeFreePlaneResources(planes);
    drmModeFreeResources(resources);
    drmModeFreeConnector(connector);
    return found;
}

uint32_t DrmDevice::GetPropertyId(uint32_t object_id, uint32_t object_type, const char* name,
                                  uint64_t* value) const {
    drmModeObjectProperties* properties =
            drmModeObjectGetProperties(fd_.get(), object_id, object_type);
    if (properties == nullptr) return 0;
    uint32_t result = 0;
    for (uint32_t i = 0; i < properties->count_props; ++i) {
        drmModePropertyRes* property = drmModeGetProperty(fd_.get(), properties->props[i]);
        if (property != nullptr && strcmp(property->name, name) == 0) {
            result = property->prop_id;
            if (value != nullptr) *value = properties->prop_values[i];
        }
        if (property != nullptr) drmModeFreeProperty(property);
        if (result != 0) break;
    }
    drmModeFreeObjectProperties(properties);
    return result;
}

bool DrmDevice::DiscoverProperties(DrmDisplay* d) {
    DrmProperties& p = d->props;
    p.connector_edid = GetPropertyId(d->connector_id, DRM_MODE_OBJECT_CONNECTOR, "EDID");
    d->orientation_degrees = GetPanelOrientation(fd_.get(), d->connector_id);
    uint64_t connector_crtc = 0;
    if (atomic_kms_) {
        p.connector_crtc_id = GetPropertyId(d->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID",
                                            &connector_crtc);
    }
    drmModeCrtc* current_crtc = drmModeGetCrtc(fd_.get(), d->crtc_id);
    if (current_crtc != nullptr && current_crtc->mode_valid) {
        const std::string current_key = ModeKey(d->connector_id, current_crtc->mode);
        for (const DrmConfig& config : d->configs) {
            if (ModeKey(d->connector_id, config.mode) == current_key) {
                d->active_config = config.id;
                if (!atomic_kms_ || connector_crtc == d->crtc_id) {
                    d->powered = true;
                    d->modeset_needed = false;
                }
                break;
            }
        }
    }
    if (current_crtc != nullptr) drmModeFreeCrtc(current_crtc);
    if (p.connector_edid != 0) {
        drmModeObjectProperties* properties =
                drmModeObjectGetProperties(fd_.get(), d->connector_id, DRM_MODE_OBJECT_CONNECTOR);
        if (properties != nullptr) {
            for (uint32_t i = 0; i < properties->count_props; ++i) {
                if (properties->props[i] != p.connector_edid) continue;
                drmModePropertyBlobRes* blob =
                        drmModeGetPropertyBlob(fd_.get(), properties->prop_values[i]);
                if (blob != nullptr && blob->data != nullptr) {
                    const auto* bytes = static_cast<const uint8_t*>(blob->data);
                    d->edid.assign(bytes, bytes + blob->length);
                }
                if (blob != nullptr) drmModeFreePropertyBlob(blob);
            }
            drmModeFreeObjectProperties(properties);
        }
        if (!IsUsableEdid(d->edid)) d->edid.clear();
    }
    if (!atomic_kms_) return true;

    uint64_t crtc_active = 0;
    p.crtc_active = GetPropertyId(d->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", &crtc_active);
    d->powered = crtc_active != 0;
    p.crtc_mode_id = GetPropertyId(d->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    p.crtc_out_fence_ptr = GetPropertyId(d->crtc_id, DRM_MODE_OBJECT_CRTC, "OUT_FENCE_PTR");
    p.plane_fb_id = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    p.plane_crtc_id = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    p.plane_src_x = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    p.plane_src_y = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    p.plane_src_w = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    p.plane_src_h = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    p.plane_crtc_x = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    p.plane_crtc_y = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    p.plane_crtc_w = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    p.plane_crtc_h = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    p.plane_in_fence_fd = GetPropertyId(d->plane_id, DRM_MODE_OBJECT_PLANE, "IN_FENCE_FD");
    return p.connector_crtc_id && p.crtc_active && p.crtc_mode_id && p.crtc_out_fence_ptr &&
           p.plane_fb_id && p.plane_crtc_id && p.plane_src_x && p.plane_src_y && p.plane_src_w &&
           p.plane_src_h && p.plane_crtc_x && p.plane_crtc_y && p.plane_crtc_w && p.plane_crtc_h;
}

bool DrmDevice::PlaneSupportsFormat(const DrmDisplay& display, uint32_t format,
                                    uint64_t modifier) const {
    if (!atomic_kms_ || display.plane_id == 0) return false;
    drmModePlane* plane = drmModeGetPlane(fd_.get(), display.plane_id);
    if (plane == nullptr) return false;
    const bool format_supported = std::find(plane->formats, plane->formats + plane->count_formats,
                                            format) != plane->formats + plane->count_formats;
    drmModeFreePlane(plane);
    if (!format_supported) return false;
    if (modifier == DRM_FORMAT_MOD_INVALID) return true;
    uint64_t blob_id = 0;
    if (GetPropertyId(display.plane_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS", &blob_id) == 0 ||
        blob_id == 0) {
        return modifier == DRM_FORMAT_MOD_NONE;
    }
    drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd_.get(), blob_id);
    if (blob == nullptr) return false;
    drmModeFormatModifierIterator iterator{};
    bool supported = false;
    while (drmModeFormatModifierBlobIterNext(blob, &iterator)) {
        if (iterator.fmt == format && iterator.mod == modifier) {
            supported = true;
            break;
        }
    }
    drmModeFreePropertyBlob(blob);
    return supported;
}

bool DrmDevice::CreateCpuSwapFramebuffer(DrmFramebuffer* fb, uint32_t format) {
    fb->cpu_swap_red_blue_ = true;
    for (size_t i = 0; i < fb->dumb_handles_.size(); ++i) {
        if (drmModeCreateDumbBuffer(fd_.get(), fb->width_, fb->height_, 32, 0,
                                    &fb->dumb_handles_[i], &fb->dumb_pitches_[i],
                                    &fb->dumb_sizes_[i]) != 0) {
            ALOGE("Failed to create CPU swap dumb buffer: %s", strerror(errno));
            return false;
        }
        uint64_t map_offset = 0;
        if (drmModeMapDumbBuffer(fd_.get(), fb->dumb_handles_[i], &map_offset) != 0) {
            ALOGE("Failed to get dumb buffer map offset: %s", strerror(errno));
            return false;
        }
        fb->dumb_maps_[i] = mmap(nullptr, fb->dumb_sizes_[i], PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd_.get(), map_offset);
        if (fb->dumb_maps_[i] == MAP_FAILED) {
            ALOGE("Failed to map CPU swap dumb buffer: %s", strerror(errno));
            return false;
        }
        std::array<uint32_t, kMaxPlanes> handles{};
        std::array<uint32_t, kMaxPlanes> pitches{};
        std::array<uint32_t, kMaxPlanes> offsets{};
        handles[0] = fb->dumb_handles_[i];
        pitches[0] = fb->dumb_pitches_[i];
        if (drmModeAddFB2(fd_.get(), fb->width_, fb->height_, format, handles.data(),
                          pitches.data(), offsets.data(), &fb->dumb_framebuffers_[i], 0) != 0) {
            ALOGE("Failed to create CPU swap framebuffer: %s", strerror(errno));
            return false;
        }
        memset(fb->dumb_maps_[i], 0, fb->dumb_sizes_[i]);
    }
    fb->id_ = fb->dumb_framebuffers_[0];
    fb->format_ = format;
    fb->modifier_ = DRM_FORMAT_MOD_NONE;
    ALOGI("Using double-buffered CPU red/blue swap fallback for FBs %u and %u",
          fb->dumb_framebuffers_[0], fb->dumb_framebuffers_[1]);
    return true;
}

std::shared_ptr<DrmFramebuffer> DrmDevice::ImportBuffer(int64_t display, buffer_handle_t handle) {
    if (handle == nullptr) return nullptr;
    auto display_it = displays_.find(display);
    if (display_it == displays_.end() || !display_it->second.connected) return nullptr;
    buffer_handle_t imported = nullptr;
    if (android::GraphicBufferMapper::get().importBufferNoValidate(handle, &imported) !=
        android::OK) {
        ALOGE("GraphicBufferMapper failed to import client target");
        return nullptr;
    }
    auto fb = std::shared_ptr<DrmFramebuffer>(new DrmFramebuffer(fd_.get()));
    fb->imported_handle_ = imported;
    fb->registry_ = gem_registry_;
    android::GraphicBufferMapper& mapper = android::GraphicBufferMapper::get();
    uint32_t format = 0;
    uint64_t modifier = DRM_FORMAT_MOD_INVALID;
    uint64_t width = 0;
    uint64_t height = 0;
    std::vector<android::ui::PlaneLayout> layouts;
    if (mapper.getPixelFormatFourCC(imported, &format) != android::OK ||
        mapper.getPixelFormatModifier(imported, &modifier) != android::OK ||
        mapper.getWidth(imported, &width) != android::OK ||
        mapper.getHeight(imported, &height) != android::OK ||
        mapper.getPlaneLayouts(imported, &layouts) != android::OK || layouts.empty() ||
        layouts.size() > kMaxPlanes || imported->numFds <= 0) {
        ALOGE("Incomplete standard mapper metadata for client target");
        return nullptr;
    }
    if (width == 0 || height == 0 || width > std::numeric_limits<int32_t>::max() ||
        height > std::numeric_limits<int32_t>::max()) {
        ALOGE("Invalid client target dimensions");
        return nullptr;
    }
    if (swap_red_blue_) {
        uint64_t protected_content = 0;
        if (mapper.getProtectedContent(imported, &protected_content) != android::OK ||
            protected_content != 0 || layouts.size() != 1) {
            ALOGE("Red/blue swap requires an unprotected single-plane client target");
            return nullptr;
        }
        const uint64_t row_bytes = width * 4;
        const auto& layout = layouts[0];
        if (layout.strideInBytes <= 0 || layout.totalSizeInBytes <= 0) {
            ALOGE("Invalid client target layout for red/blue swap");
            return nullptr;
        }
        const uint64_t stride = static_cast<uint64_t>(layout.strideInBytes);
        const uint64_t size = static_cast<uint64_t>(layout.totalSizeInBytes);
        if (stride < row_bytes || stride > std::numeric_limits<uint32_t>::max() ||
            size < row_bytes || (height - 1) * stride + row_bytes > size) {
            ALOGE("Invalid client target layout for red/blue swap");
            return nullptr;
        }
    }
    std::array<uint32_t, kMaxPlanes> pitches{};
    std::array<uint32_t, kMaxPlanes> offsets{};
    std::array<uint64_t, kMaxPlanes> modifiers{};
    fb->width_ = width;
    fb->height_ = height;
    fb->source_format_ = format;
    fb->source_stride_ = layouts[0].strideInBytes;
    fb->source_size_ = layouts[0].totalSizeInBytes;
    const uint32_t swapped_format = swap_red_blue_ ? SwapRedBlueFormat(format) : 0;
    if (swap_red_blue_ && swapped_format == 0) {
        ALOGE("Red/blue swap is unsupported for DRM format %08x", format);
        return nullptr;
    }
    const bool cpu_swap =
            swap_red_blue_ &&
            (!atomic_kms_ || !PlaneSupportsFormat(display_it->second, swapped_format, modifier));
    if (cpu_swap) {
        if (layouts.size() != 1 || !CreateCpuSwapFramebuffer(fb.get(), format)) return nullptr;
        return fb;
    }
    int fd_index = 0;
    std::set<uint32_t> acquired_handles;
    for (size_t i = 0; i < layouts.size(); ++i) {
        if (layouts[i].strideInBytes <= 0 ||
            layouts[i].strideInBytes > std::numeric_limits<uint32_t>::max() ||
            layouts[i].offsetInBytes < 0 ||
            layouts[i].offsetInBytes > std::numeric_limits<uint32_t>::max()) {
            ALOGE("Invalid mapper layout for plane %zu", i);
            return nullptr;
        }
        if (i != 0 && layouts[i].offsetInBytes == 0) ++fd_index;
        if (fd_index >= imported->numFds || imported->data[fd_index] < 0 ||
            drmPrimeFDToHandle(fd_.get(), imported->data[fd_index], &fb->gem_handles_[i]) != 0) {
            ALOGE("Cannot infer/import dma-buf for plane %zu", i);
            return nullptr;
        }
        if (acquired_handles.insert(fb->gem_handles_[i]).second) {
            gem_registry_->Acquire(fb->gem_handles_[i]);
        }
        pitches[i] = layouts[i].strideInBytes;
        offsets[i] = layouts[i].offsetInBytes;
        modifiers[i] = modifier;
    }
    const uint32_t scanout_format = swap_red_blue_ ? swapped_format : format;
    const bool has_modifier = modifier != DRM_FORMAT_MOD_NONE && modifier != DRM_FORMAT_MOD_INVALID;
    int error;
    if (has_modifier) {
        if (!modifiers_supported_) {
            if (swap_red_blue_ && layouts.size() == 1 &&
                CreateCpuSwapFramebuffer(fb.get(), format)) {
                return fb;
            }
            ALOGE("Buffer requires modifier support (modifier=%" PRIu64 ")", modifier);
            return nullptr;
        }
        error = drmModeAddFB2WithModifiers(fd_.get(), width, height, scanout_format,
                                           fb->gem_handles_.data(), pitches.data(), offsets.data(),
                                           modifiers.data(), &fb->id_, DRM_MODE_FB_MODIFIERS);
    } else {
        error = drmModeAddFB2(fd_.get(), width, height, scanout_format, fb->gem_handles_.data(),
                              pitches.data(), offsets.data(), &fb->id_, 0);
    }
    if (error != 0) {
        if (swap_red_blue_ && layouts.size() == 1 && CreateCpuSwapFramebuffer(fb.get(), format)) {
            return fb;
        }
        ALOGE("drmModeAddFB2 failed: %s", strerror(errno));
        return nullptr;
    }
    fb->format_ = scanout_format;
    fb->modifier_ = modifier;
    ALOGV("Imported FB %u %ux%u fourcc=%08x modifier=%" PRIu64, fb->id_, fb->width_, fb->height_,
          scanout_format, modifier);
    return fb;
}

bool DrmDevice::PrepareFramebuffer(const std::shared_ptr<DrmFramebuffer>& fb, int acquire_fence,
                                   int* scanout_fence) {
    *scanout_fence = acquire_fence;
    if (fb == nullptr || !fb->cpu_swap_red_blue_) return true;
    if (acquire_fence >= 0) {
        constexpr int kAcquireFenceTimeoutMs = 3000;
        if (sync_wait(acquire_fence, kAcquireFenceTimeoutMs) != 0) {
            ALOGE("Acquire fence timed out before CPU red/blue swap");
            return false;
        }
    }
    const size_t next_index = (fb->dumb_index_ + 1) % fb->dumb_handles_.size();
    const uint64_t row_bytes = static_cast<uint64_t>(fb->width_) * 4;
    const uint64_t source_end =
            static_cast<uint64_t>(fb->height_ - 1) * fb->source_stride_ + row_bytes;
    const uint64_t destination_end =
            static_cast<uint64_t>(fb->height_ - 1) * fb->dumb_pitches_[next_index] + row_bytes;
    if (fb->source_stride_ < row_bytes || fb->dumb_pitches_[next_index] < row_bytes ||
        source_end > fb->source_size_ || destination_end > fb->dumb_sizes_[next_index] ||
        fb->dumb_maps_[next_index] == nullptr || fb->dumb_maps_[next_index] == MAP_FAILED) {
        ALOGE("Invalid buffer bounds for CPU red/blue swap");
        return false;
    }
    void* source = nullptr;
    const android::Rect bounds(0, 0, static_cast<int32_t>(fb->width_),
                               static_cast<int32_t>(fb->height_));
    android::GraphicBufferMapper& mapper = android::GraphicBufferMapper::get();
    if (mapper.lock(fb->imported_handle_, GRALLOC_USAGE_SW_READ_OFTEN, bounds, &source) !=
        android::OK) {
        ALOGE("Unable to map client target for CPU red/blue swap");
        return false;
    }
    if (source == nullptr) {
        mapper.unlock(fb->imported_handle_);
        ALOGE("Client target mapping returned no address for CPU red/blue swap");
        return false;
    }
    const auto* source_bytes = static_cast<const uint8_t*>(source);
    auto* destination_bytes = static_cast<uint8_t*>(fb->dumb_maps_[next_index]);
    for (uint32_t y = 0; y < fb->height_; ++y) {
        const uint64_t source_offset = static_cast<uint64_t>(y) * fb->source_stride_;
        const uint64_t destination_offset =
                static_cast<uint64_t>(y) * fb->dumb_pitches_[next_index];
        const uint8_t* source_row = source_bytes + static_cast<size_t>(source_offset);
        uint8_t* destination_row = destination_bytes + static_cast<size_t>(destination_offset);
        for (uint32_t x = 0; x < fb->width_; ++x) {
            destination_row[x * 4] = source_row[x * 4 + 2];
            destination_row[x * 4 + 1] = source_row[x * 4 + 1];
            destination_row[x * 4 + 2] = source_row[x * 4];
            destination_row[x * 4 + 3] = source_row[x * 4 + 3];
        }
    }
    android::base::unique_fd unlock_fence;
    const android::status_t unlock_result = mapper.unlock(fb->imported_handle_, &unlock_fence);
    if (unlock_result != android::OK) {
        ALOGE("Unable to unlock client target after CPU red/blue swap");
        return false;
    }
    constexpr int kUnlockFenceTimeoutMs = 3000;
    if (unlock_fence.ok() && sync_wait(unlock_fence.get(), kUnlockFenceTimeoutMs) != 0) {
        ALOGE("CPU red/blue swap unlock fence failed");
        return false;
    }
    fb->prepared_dumb_index_ = next_index;
    fb->id_ = fb->dumb_framebuffers_[next_index];
    const int dirty_result = drmModeDirtyFB(fd_.get(), fb->id_, nullptr, 0);
    if (dirty_result != 0 && dirty_result != -ENOSYS && dirty_result != -EINVAL) {
        ALOGW("Failed to mark CPU swap framebuffer dirty: %s", strerror(errno));
    }
    *scanout_fence = -1;
    return true;
}

bool DrmDevice::AddProperty(drmModeAtomicReq* request, uint32_t object_id, uint32_t property_id,
                            uint64_t value) const {
    return property_id != 0 &&
           drmModeAtomicAddProperty(request, object_id, property_id, value) >= 0;
}

bool DrmDevice::AtomicCommit(DrmDisplay* d, const std::shared_ptr<DrmFramebuffer>& fb,
                             int acquire_fence, bool test_only,
                             android::base::unique_fd* out_fence) {
    if (d == nullptr) return false;
    auto config = std::find_if(d->configs.begin(), d->configs.end(),
                               [d](const DrmConfig& c) { return c.id == d->active_config; });
    if (config == d->configs.end()) return false;
    const bool modeset = d->modeset_needed || !d->powered;
    uint32_t mode_blob = 0;
    if (modeset &&
        drmModeCreatePropertyBlob(fd_.get(), &config->mode, sizeof(config->mode), &mode_blob) != 0)
        return false;
    drmModeAtomicReq* request = drmModeAtomicAlloc();
    int fence = -1;
    const uint32_t width = config->mode.hdisplay;
    const uint32_t height = config->mode.vdisplay;
    bool ok = request != nullptr && AddProperty(request, d->crtc_id, d->props.crtc_out_fence_ptr,
                                                reinterpret_cast<uint64_t>(&fence));
    if (ok && modeset) {
        ok = AddProperty(request, d->connector_id, d->props.connector_crtc_id, d->crtc_id) &&
             AddProperty(request, d->crtc_id, d->props.crtc_active, 1) &&
             AddProperty(request, d->crtc_id, d->props.crtc_mode_id, mode_blob);
    }
    if (ok && fb != nullptr) {
        ok = AddProperty(request, d->plane_id, d->props.plane_fb_id, fb->id()) &&
             AddProperty(request, d->plane_id, d->props.plane_crtc_id, d->crtc_id) &&
             AddProperty(request, d->plane_id, d->props.plane_src_x, 0) &&
             AddProperty(request, d->plane_id, d->props.plane_src_y, 0) &&
             AddProperty(request, d->plane_id, d->props.plane_src_w,
                         static_cast<uint64_t>(fb->width()) << 16) &&
             AddProperty(request, d->plane_id, d->props.plane_src_h,
                         static_cast<uint64_t>(fb->height()) << 16) &&
             AddProperty(request, d->plane_id, d->props.plane_crtc_x, 0) &&
             AddProperty(request, d->plane_id, d->props.plane_crtc_y, 0) &&
             AddProperty(request, d->plane_id, d->props.plane_crtc_w, width) &&
             AddProperty(request, d->plane_id, d->props.plane_crtc_h, height);
    } else if (ok) {
        ok = AddProperty(request, d->plane_id, d->props.plane_fb_id, 0) &&
             AddProperty(request, d->plane_id, d->props.plane_crtc_id, 0);
    }
    if (ok && fb != nullptr && acquire_fence >= 0 && d->props.plane_in_fence_fd != 0) {
        ok = AddProperty(request, d->plane_id, d->props.plane_in_fence_fd, acquire_fence);
    } else if (ok && acquire_fence >= 0 && !test_only) {
        constexpr int kAcquireFenceTimeoutMs = 3000;
        ok = sync_wait(acquire_fence, kAcquireFenceTimeoutMs) == 0;
    }
    const uint32_t flags = (modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0) |
                           (test_only ? DRM_MODE_ATOMIC_TEST_ONLY : 0);
    if (ok && drmModeAtomicCommit(fd_.get(), request, flags, nullptr) != 0) {
        ALOGE("Atomic %s failed for display %" PRId64 ": %s", test_only ? "test" : "present", d->id,
              strerror(errno));
        ok = false;
    }
    if (request != nullptr) drmModeAtomicFree(request);
    if (mode_blob != 0) drmModeDestroyPropertyBlob(fd_.get(), mode_blob);
    if (test_only) {
        if (fence >= 0) close(fence);
    } else if (ok && fence >= 0) {
        out_fence->reset(fence);
        d->powered = true;
        d->modeset_needed = false;
    } else {
        if (fence >= 0) close(fence);
        ok = false;
        ALOGE("Atomic present did not return a valid OUT_FENCE_PTR fence");
    }
    return ok;
}

bool DrmDevice::Test(int64_t display, const std::shared_ptr<DrmFramebuffer>& fb,
                     int acquire_fence) {
    auto it = displays_.find(display);
    if (it == displays_.end() || !it->second.connected) return false;
    if (!atomic_kms_) {
        const DrmDisplay& d = it->second;
        const auto config =
                std::find_if(d.configs.begin(), d.configs.end(),
                             [&d](const DrmConfig& item) { return item.id == d.active_config; });
        return config != d.configs.end() &&
               (fb == nullptr ||
                (fb->width() == config->mode.hdisplay && fb->height() == config->mode.vdisplay));
    }
    android::base::unique_fd unused;
    return AtomicCommit(&it->second, fb, acquire_fence, true, &unused);
}

bool DrmDevice::TestConfiguration(int64_t display) {
    auto it = displays_.find(display);
    if (it == displays_.end() || !it->second.connected) return false;
    DrmDisplay& d = it->second;
    if (!atomic_kms_) {
        return std::any_of(d.configs.begin(), d.configs.end(),
                           [&d](const DrmConfig& item) { return item.id == d.active_config; });
    }
    drmModeAtomicReq* request = drmModeAtomicAlloc();
    uint32_t mode_blob = 0;
    bool ok = request != nullptr;
    if (d.powered) {
        const auto config =
                std::find_if(d.configs.begin(), d.configs.end(),
                             [&d](const DrmConfig& item) { return item.id == d.active_config; });
        ok = ok && config != d.configs.end() &&
             drmModeCreatePropertyBlob(fd_.get(), &config->mode, sizeof(config->mode),
                                       &mode_blob) == 0 &&
             AddProperty(request, d.connector_id, d.props.connector_crtc_id, d.crtc_id) &&
             AddProperty(request, d.crtc_id, d.props.crtc_mode_id, mode_blob) &&
             AddProperty(request, d.crtc_id, d.props.crtc_active, 1);
    } else {
        ok = ok && AddProperty(request, d.connector_id, d.props.connector_crtc_id, 0) &&
             AddProperty(request, d.crtc_id, d.props.crtc_mode_id, 0) &&
             AddProperty(request, d.crtc_id, d.props.crtc_active, 0);
    }
    ok = ok && drmModeAtomicCommit(fd_.get(), request,
                                   DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY,
                                   nullptr) == 0;
    if (request != nullptr) drmModeAtomicFree(request);
    if (mode_blob != 0) drmModeDestroyPropertyBlob(fd_.get(), mode_blob);
    if (!ok) {
        ALOGE("Atomic configuration test failed for display %" PRId64 ": %s", display,
              strerror(errno));
    }
    return ok;
}

bool DrmDevice::LegacyPresent(DrmDisplay* d, const std::shared_ptr<DrmFramebuffer>& fb,
                              int acquire_fence) {
    if (d == nullptr) return false;
    if (fb == nullptr) {
        if (!d->has_legacy_framebuffer) return true;
        if (drmModeSetCrtc(fd_.get(), d->crtc_id, 0, 0, 0, nullptr, 0, nullptr) != 0) {
            ALOGE("Failed to clear legacy display %" PRId64 ": %s", d->id, strerror(errno));
            return false;
        }
        d->has_legacy_framebuffer = false;
        d->modeset_needed = true;
        return true;
    }
    const auto config =
            std::find_if(d->configs.begin(), d->configs.end(),
                         [d](const DrmConfig& item) { return item.id == d->active_config; });
    if (config == d->configs.end() || fb->width() != config->mode.hdisplay ||
        fb->height() != config->mode.vdisplay) {
        ALOGE("Legacy framebuffer dimensions do not match display %" PRId64, d->id);
        return false;
    }
    if (acquire_fence >= 0) {
        constexpr int kAcquireFenceTimeoutMs = 3000;
        if (sync_wait(acquire_fence, kAcquireFenceTimeoutMs) != 0) {
            ALOGE("Acquire fence timed out for legacy display %" PRId64, d->id);
            return false;
        }
    }
    const bool framebuffer_changed =
            d->has_legacy_framebuffer &&
            (d->legacy_format != fb->format() || d->legacy_modifier != fb->modifier());
    if (d->modeset_needed || !d->powered || framebuffer_changed) {
        uint32_t connector = d->connector_id;
        if (drmModeSetCrtc(fd_.get(), d->crtc_id, fb->id(), 0, 0, &connector, 1, &config->mode) !=
            0) {
            ALOGE("Legacy modeset failed for display %" PRId64 ": %s", d->id, strerror(errno));
            return false;
        }
        d->powered = true;
        d->modeset_needed = false;
        d->has_legacy_framebuffer = true;
        d->legacy_format = fb->format();
        d->legacy_modifier = fb->modifier();
        return true;
    }
    std::lock_guard event_lock(event_mutex_);
    PageFlipResult result;
    if (drmModePageFlip(fd_.get(), d->crtc_id, fb->id(), DRM_MODE_PAGE_FLIP_EVENT, &result) != 0) {
        ALOGE("Legacy page flip failed for display %" PRId64 ": %s", d->id, strerror(errno));
        return false;
    }
    while (!result.complete) {
        pollfd drm_poll{fd_.get(), POLLIN, 0};
        const int poll_result = poll(&drm_poll, 1, -1);
        if (poll_result < 0 && errno == EINTR) continue;
        if (poll_result <= 0 || (drm_poll.revents & POLLIN) == 0) {
            ALOGE("Legacy page flip event wait failed for display %" PRId64 ": %s", d->id,
                  strerror(errno));
            continue;
        }
        drmEventContext context{};
        context.version = DRM_EVENT_CONTEXT_VERSION;
        context.page_flip_handler2 = HandlePageFlip;
        if (drmHandleEvent(fd_.get(), &context) != 0) {
            ALOGE("Legacy page flip event handling failed for display %" PRId64 ": %s", d->id,
                  strerror(errno));
        }
    }
    return true;
}

android::base::unique_fd DrmDevice::CreateSignaledFence() const {
    if (!syncobj_supported_) return {};
    uint32_t syncobj = 0;
    if (drmSyncobjCreate(fd_.get(), DRM_SYNCOBJ_CREATE_SIGNALED, &syncobj) != 0) {
        ALOGW("Failed to create signaled DRM syncobj: %s", strerror(errno));
        return {};
    }
    int fence = -1;
    const int export_result = drmSyncobjExportSyncFile(fd_.get(), syncobj, &fence);
    drmSyncobjDestroy(fd_.get(), syncobj);
    if (export_result != 0 || fence < 0) {
        if (fence >= 0) close(fence);
        ALOGW("Failed to export signaled DRM syncobj: %s", strerror(errno));
        return {};
    }
    return android::base::unique_fd(fence);
}

bool DrmDevice::Present(int64_t display, const std::shared_ptr<DrmFramebuffer>& fb,
                        int acquire_fence, android::base::unique_fd* out_fence) {
    auto it = displays_.find(display);
    if (it == displays_.end() || !it->second.connected) return false;
    int scanout_fence = acquire_fence;
    if (!PrepareFramebuffer(fb, acquire_fence, &scanout_fence)) return false;
    out_fence->reset();
    if (!atomic_kms_) {
        if (!LegacyPresent(&it->second, fb, scanout_fence)) return false;
        *out_fence = CreateSignaledFence();
        if (!out_fence->ok() && acquire_fence >= 0) out_fence->reset(dup(acquire_fence));
        if (!out_fence->ok()) {
            ALOGW("Legacy present completed without an exportable sync-file fence");
        }
        if (fb != nullptr && fb->cpu_swap_red_blue_) {
            fb->dumb_index_ = fb->prepared_dumb_index_;
        }
        return true;
    }
    const bool presented = AtomicCommit(&it->second, fb, scanout_fence, true, out_fence) &&
                           AtomicCommit(&it->second, fb, scanout_fence, false, out_fence);
    if (presented && fb != nullptr && fb->cpu_swap_red_blue_) {
        fb->dumb_index_ = fb->prepared_dumb_index_;
    }
    return presented;
}

bool DrmDevice::SetPower(int64_t display, bool on) {
    auto it = displays_.find(display);
    if (it == displays_.end() || !it->second.connected) return false;
    DrmDisplay& d = it->second;
    if (d.powered == on) return true;
    if (!atomic_kms_) {
        bool ok = true;
        if (on) {
            d.powered = true;
            d.modeset_needed = true;
        } else {
            ok = drmModeSetCrtc(fd_.get(), d.crtc_id, 0, 0, 0, nullptr, 0, nullptr) == 0;
            if (ok) {
                d.powered = false;
                d.modeset_needed = true;
                d.has_legacy_framebuffer = false;
            }
        }
        ALOGI("Legacy display %" PRId64 " power %s %s", display, on ? "ON" : "OFF",
              ok ? "succeeded" : "failed");
        return ok;
    }
    drmModeAtomicReq* request = drmModeAtomicAlloc();
    uint32_t mode_blob = 0;
    bool ok = request != nullptr;
    if (on) {
        const auto config =
                std::find_if(d.configs.begin(), d.configs.end(),
                             [&d](const DrmConfig& item) { return item.id == d.active_config; });
        ok = ok && config != d.configs.end() &&
             drmModeCreatePropertyBlob(fd_.get(), &config->mode, sizeof(config->mode),
                                       &mode_blob) == 0 &&
             AddProperty(request, d.connector_id, d.props.connector_crtc_id, d.crtc_id) &&
             AddProperty(request, d.crtc_id, d.props.crtc_mode_id, mode_blob) &&
             AddProperty(request, d.crtc_id, d.props.crtc_active, 1);
    } else {
        ok = ok && AddProperty(request, d.plane_id, d.props.plane_fb_id, 0) &&
             AddProperty(request, d.plane_id, d.props.plane_crtc_id, 0) &&
             AddProperty(request, d.connector_id, d.props.connector_crtc_id, 0) &&
             AddProperty(request, d.crtc_id, d.props.crtc_active, 0) &&
             AddProperty(request, d.crtc_id, d.props.crtc_mode_id, 0);
    }
    ok = ok && drmModeAtomicCommit(fd_.get(), request, DRM_MODE_ATOMIC_ALLOW_MODESET, nullptr) == 0;
    if (request != nullptr) drmModeAtomicFree(request);
    if (mode_blob != 0) drmModeDestroyPropertyBlob(fd_.get(), mode_blob);
    if (ok) {
        d.powered = on;
        d.modeset_needed = !on;
    }
    ALOGI("Display %" PRId64 " power %s %s", display, on ? "ON" : "OFF",
          ok ? "succeeded" : "failed");
    return ok;
}

bool DrmDevice::SetActiveConfig(int64_t display, int32_t config) {
    auto it = displays_.find(display);
    if (it == displays_.end()) return false;
    auto found = std::find_if(it->second.configs.begin(), it->second.configs.end(),
                              [config](const DrmConfig& c) { return c.id == config; });
    if (found == it->second.configs.end()) return false;
    DrmDisplay& d = it->second;
    const int32_t old_config = d.active_config;
    const bool was_powered = d.powered;
    if (was_powered && !SetPower(display, false)) return false;
    d.active_config = config;
    if (was_powered) {
        if (!SetPower(display, true)) {
            d.active_config = old_config;
            SetPower(display, true);
            return false;
        }
    }
    return true;
}

std::string DrmDevice::Dump() const {
    std::ostringstream out;
    out << "DRM fd=" << fd_.get() << " backend=" << (atomic_kms_ ? "atomic" : "legacy")
        << " modifiers=" << modifiers_supported_ << " syncobj=" << syncobj_supported_
        << " swapRb=" << swap_red_blue_ << '\n';
    for (const auto& [id, d] : displays_) {
        out << " display=" << id << " " << d.name << " connected=" << d.connected
            << " internal=" << d.internal << " power=" << d.powered
            << " connector=" << d.connector_id << " crtc=" << d.crtc_id << " plane=" << d.plane_id
            << " activeConfig=" << d.active_config << " modesetNeeded=" << d.modeset_needed
            << " configs=" << d.configs.size() << '\n';
    }
    return out.str();
}

}  // namespace drmfb
