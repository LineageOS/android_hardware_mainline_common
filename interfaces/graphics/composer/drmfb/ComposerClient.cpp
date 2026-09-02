/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "drmfb-composer3"

#include "ComposerClient.h"

#include <aidl/android/hardware/graphics/common/DisplayHotplugEvent.h>
#include <aidl/android/hardware/graphics/composer3/ChangedCompositionTypes.h>
#include <aidl/android/hardware/graphics/composer3/CommandError.h>
#include <aidl/android/hardware/graphics/composer3/PresentFence.h>
#include <aidl/android/hardware/graphics/composer3/PresentOrValidate.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/stringprintf.h>
#include <android/binder_ibinder_platform.h>
#include <cutils/native_handle.h>
#include <drm/drm.h>
#include <linux/netlink.h>
#include <log/log.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace aidl::android::hardware::graphics::composer3::impl {
namespace {

constexpr int32_t kBadConfig = IComposerClient::EX_BAD_CONFIG;
constexpr int32_t kBadDisplay = IComposerClient::EX_BAD_DISPLAY;
constexpr int32_t kBadLayer = IComposerClient::EX_BAD_LAYER;
constexpr int32_t kBadParameter = IComposerClient::EX_BAD_PARAMETER;
constexpr int32_t kNoResources = IComposerClient::EX_NO_RESOURCES;
constexpr int32_t kNotValidated = IComposerClient::EX_NOT_VALIDATED;
constexpr int32_t kUnsupported = IComposerClient::EX_UNSUPPORTED;
constexpr int32_t kMaxBufferSlots = 4096;

const drmfb::DrmConfig* FindConfig(const drmfb::DrmDisplay& display, int32_t id) {
    auto it = std::find_if(display.configs.begin(), display.configs.end(),
                           [id](const drmfb::DrmConfig& config) { return config.id == id; });
    return it == display.configs.end() ? nullptr : &*it;
}

int32_t ModePeriod(const drmModeModeInfo& mode) {
    uint64_t total = static_cast<uint64_t>(mode.htotal) * mode.vtotal;
    if ((mode.flags & DRM_MODE_FLAG_INTERLACE) != 0) total /= 2;
    if ((mode.flags & DRM_MODE_FLAG_DBLSCAN) != 0) total *= 2;
    total *= std::max<uint16_t>(mode.vscan, 1);
    return mode.clock == 0 ? 16666666 : static_cast<int32_t>(total * 1000000ULL / mode.clock);
}

struct SequenceResult {
    uint64_t timestamp_ns = 0;
};

void HandleSequence(int, uint64_t, uint64_t ns, uint64_t user_data) {
    auto* result = reinterpret_cast<SequenceResult*>(user_data);
    result->timestamp_ns = ns;
}

DisplayConfiguration ToAidlConfig(const drmfb::DrmDisplay& display,
                                  const drmfb::DrmConfig& config) {
    DisplayConfiguration out;
    out.configId = config.id;
    out.width = config.mode.hdisplay;
    out.height = config.mode.vdisplay;
    out.configGroup = config.group;
    out.vsyncPeriod = ModePeriod(config.mode);
    out.hdrOutputType = OutputType::SDR;
    if (display.mm_width > 0 && display.mm_height > 0) {
        constexpr float kMillimetersPerInch = 25.4F;
        out.dpi = DisplayConfiguration::Dpi{
                .x = out.width * kMillimetersPerInch / display.mm_width,
                .y = out.height * kMillimetersPerInch / display.mm_height};
    }
    return out;
}

}  // namespace

ComposerClient::ComposerClient(std::string drm_path) : drm_path_(std::move(drm_path)) {}

ComposerClient::~ComposerClient() {
    stopping_ = true;
    vsync_cv_.notify_all();
    if (wake_fd_.ok()) {
        uint64_t one = 1;
        if (write(wake_fd_.get(), &one, sizeof(one)) < 0) {
            ALOGV("Hotplug wake write failed: %s", strerror(errno));
        }
    }
    if (hotplug_thread_.joinable()) hotplug_thread_.join();
    if (vblank_thread_.joinable()) vblank_thread_.join();
    std::lock_guard lock(mutex_);
    callback_.reset();
    states_.clear();
    ALOGI("Composer client stopped");
}

bool ComposerClient::Init() {
    if (!drm_.Init(drm_path_)) return false;
    for (const auto& [id, display] : drm_.displays()) {
        if (!display.connected) continue;
        if (display.powered && !drm_.SetPower(id, false)) {
            ALOGE("Failed to establish initial OFF state for display %" PRId64, id);
            return false;
        }
        states_.emplace(id, DisplayState{});
    }
    wake_fd_.reset(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!wake_fd_.ok()) {
        ALOGE("Cannot create hotplug worker wake event: %s", strerror(errno));
        return false;
    }
    hotplug_socket_.reset(socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT));
    if (hotplug_socket_.ok()) {
        sockaddr_nl address{};
        address.nl_family = AF_NETLINK;
        address.nl_pid = getpid();
        address.nl_groups = 0xffffffff;
        int receive_buffer = 64 * 1024;
        setsockopt(hotplug_socket_.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                   sizeof(receive_buffer));
        if (bind(hotplug_socket_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
            0) {
            ALOGW("Cannot bind DRM uevent socket: %s", strerror(errno));
            hotplug_socket_.reset();
        }
    }
    hotplug_thread_ = std::thread(&ComposerClient::HotplugLoop, this);
    vblank_thread_ = std::thread(&ComposerClient::VblankLoop, this);
    return true;
}

ndk::ScopedAStatus ComposerClient::Error(int32_t error) {
    return ndk::ScopedAStatus::fromServiceSpecificError(error);
}

ndk::ScopedAStatus ComposerClient::UnsupportedDisplay(int64_t display) {
    std::lock_guard lock(mutex_);
    return FindDisplayLocked(display) == nullptr ? Error(kBadDisplay) : Error(kUnsupported);
}

drmfb::DrmDisplay* ComposerClient::FindDisplayLocked(int64_t display) {
    auto it = drm_.displays().find(display);
    return it == drm_.displays().end() || !it->second.connected ? nullptr : &it->second;
}

ComposerClient::DisplayState* ComposerClient::FindStateLocked(int64_t display) {
    auto it = states_.find(display);
    return FindDisplayLocked(display) == nullptr || it == states_.end() ? nullptr : &it->second;
}

ndk::ScopedAStatus ComposerClient::createLayer(int64_t display, int32_t slot_count,
                                               int64_t* layer) {
    if (slot_count < 0) return Error(kBadParameter);
    if (slot_count > kMaxBufferSlots) return Error(kNoResources);
    std::lock_guard lock(mutex_);
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) return Error(kBadDisplay);
    *layer = state->next_layer++;
    state->layers.emplace(*layer, LayerState{.buffer_slot_count = slot_count});
    state->validation = ValidationState::kDirty;
    ALOGV("Created display=%" PRId64 " layer=%" PRId64, display, *layer);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::createVirtualDisplay(int32_t, int32_t, common::PixelFormat,
                                                        int32_t, VirtualDisplay*) {
    return Error(kUnsupported);
}

ndk::ScopedAStatus ComposerClient::destroyLayer(int64_t display, int64_t layer) {
    std::lock_guard lock(mutex_);
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) return Error(kBadDisplay);
    if (state->layers.erase(layer) == 0) return Error(kBadLayer);
    state->validation = ValidationState::kDirty;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::destroyVirtualDisplay(int64_t display) {
    std::lock_guard lock(mutex_);
    return FindDisplayLocked(display) == nullptr ? Error(kBadDisplay) : Error(kBadParameter);
}

void ComposerClient::AddError(size_t index, int32_t error,
                              std::vector<CommandResultPayload>* results) {
    CommandError command_error;
    command_error.commandIndex = static_cast<int32_t>(index);
    command_error.errorCode = error;
    results->emplace_back(command_error);
}

bool ComposerClient::ApplyLayerLocked(int64_t display, const LayerCommand& command,
                                      int32_t* error) {
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) {
        *error = kBadDisplay;
        return false;
    }
    switch (command.layerLifecycleBatchCommandType) {
        case LayerLifecycleBatchCommandType::CREATE: {
            if (command.newBufferSlotCount < 0 || command.newBufferSlotCount > kMaxBufferSlots ||
                state->layers.count(command.layer) != 0) {
                if (command.newBufferSlotCount > kMaxBufferSlots) {
                    *error = kNoResources;
                    return false;
                }
                *error = command.newBufferSlotCount < 0 ? kBadParameter : kBadLayer;
                return false;
            }
            state->layers.emplace(command.layer,
                                  LayerState{.buffer_slot_count = command.newBufferSlotCount});
            if (command.layer >= state->next_layer &&
                command.layer != std::numeric_limits<int64_t>::max()) {
                state->next_layer = command.layer + 1;
            }
            state->validation = ValidationState::kDirty;
            break;
        }
        case LayerLifecycleBatchCommandType::DESTROY:
            if (state->layers.erase(command.layer) == 0) {
                *error = kBadLayer;
                return false;
            }
            state->validation = ValidationState::kDirty;
            return true;
        case LayerLifecycleBatchCommandType::MODIFY:
            break;
        default:
            *error = kBadParameter;
            return false;
    }
    auto layer = state->layers.find(command.layer);
    if (layer == state->layers.end()) {
        *error = kBadLayer;
        return false;
    }
    const Composition requested_composition =
            command.composition ? command.composition->composition : layer->second.composition;
    if (command.buffer && requested_composition != Composition::CLIENT &&
        requested_composition != Composition::SOLID_COLOR &&
        requested_composition != Composition::SIDEBAND &&
        (command.buffer->slot < 0 || command.buffer->slot >= layer->second.buffer_slot_count)) {
        *error = kBadParameter;
        return false;
    }
    if (command.bufferSlotsToClear) {
        for (int32_t slot : *command.bufferSlotsToClear) {
            if (slot < 0 || slot >= layer->second.buffer_slot_count) {
                *error = kBadParameter;
                return false;
            }
        }
    }
    if (command.luts) {
        *error = kUnsupported;
        return false;
    }
    if (command.composition) {
        switch (command.composition->composition) {
            case Composition::CLIENT:
            case Composition::DEVICE:
            case Composition::SOLID_COLOR:
            case Composition::CURSOR:
            case Composition::REFRESH_RATE_INDICATOR:
                break;
            case Composition::SIDEBAND:
            case Composition::DISPLAY_DECORATION:
                *error = kUnsupported;
                return false;
            default:
                *error = kBadParameter;
                return false;
        }
        layer->second.composition = command.composition->composition;
    }
    if (command.sidebandStream && layer->second.composition == Composition::SIDEBAND) {
        *error = kUnsupported;
        return false;
    }
    if (command.brightness &&
        (!std::isfinite(command.brightness->brightness) || command.brightness->brightness < 0.0F ||
         command.brightness->brightness > 1.0F)) {
        *error = kBadParameter;
        return false;
    }
    if (command.planeAlpha &&
        (!std::isfinite(command.planeAlpha->alpha) || command.planeAlpha->alpha < 0.0F ||
         command.planeAlpha->alpha > 1.0F)) {
        *error = kBadParameter;
        return false;
    }
    const bool state_update =
            command.cursorPosition || command.blendMode || command.color || command.composition ||
            command.dataspace || command.displayFrame || command.planeAlpha || command.sourceCrop ||
            command.transform || command.visibleRegion || command.z || command.colorTransform ||
            command.brightness || command.perFrameMetadata || command.perFrameMetadataBlob ||
            command.blockingRegion || command.luts || command.pictureProfileId != 0;
    if (state_update) state->validation = ValidationState::kDirty;
    // Layer buffers and damage are intentionally ignored for CLIENT composition.
    return true;
}

bool ComposerClient::SetClientTargetLocked(int64_t display, const ClientTarget& target,
                                           int32_t* error) {
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) {
        *error = kBadDisplay;
        return false;
    }
    const Buffer& buffer = target.buffer;
    if (buffer.slot < 0 || static_cast<size_t>(buffer.slot) >= state->target_slots.size()) {
        *error = kBadParameter;
        return false;
    }
    if (buffer.handle) {
        native_handle_t* raw = ::android::makeFromAidl(*buffer.handle);
        if (raw == nullptr) {
            *error = kBadParameter;
            return false;
        }
        auto imported = drm_.ImportBuffer(display, raw);
        native_handle_delete(raw);
        if (imported == nullptr) {
            *error = kBadParameter;
            return false;
        }
        state->target_slots[buffer.slot] = std::move(imported);
    }
    state->target = state->target_slots[buffer.slot];
    // An empty target is valid when no layer needs client composition.
    if (state->target == nullptr && buffer.handle) {
        *error = kBadParameter;
        return false;
    }
    state->target_full_damage = target.damage.empty();
    state->target_damage.clear();
    for (const common::Rect& rect : target.damage) {
        if (rect.left < 0 || rect.top < 0 || rect.right < rect.left || rect.bottom < rect.top ||
            (state->target != nullptr &&
             (rect.right > static_cast<int32_t>(state->target->width()) ||
              rect.bottom > static_cast<int32_t>(state->target->height())))) {
            *error = kBadParameter;
            return false;
        }
        if (rect.left == rect.right || rect.top == rect.bottom) continue;
        state->target_damage.push_back(
                {.left = rect.left, .top = rect.top, .right = rect.right, .bottom = rect.bottom});
    }
    if (buffer.fence.get() >= 0) {
        const int fence = dup(buffer.fence.get());
        if (fence < 0) {
            *error = kNoResources;
            return false;
        }
        state->target_fence.reset(fence);
    } else {
        state->target_fence.reset();
    }
    return true;
}

void ComposerClient::ValidateLocked(int64_t display, std::vector<CommandResultPayload>* results) {
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) return;
    ChangedCompositionTypes changes;
    changes.display = display;
    for (const auto& [layer_id, layer] : state->layers) {
        if (layer.composition == Composition::CLIENT) continue;
        ChangedCompositionLayer changed;
        changed.layer = layer_id;
        changed.composition = Composition::CLIENT;
        changes.layers.push_back(changed);
    }
    state->validation =
            changes.layers.empty() ? ValidationState::kValidated : ValidationState::kAwaitingAccept;
    if (!changes.layers.empty()) results->emplace_back(std::move(changes));
}

bool ComposerClient::PresentLocked(int64_t display, std::vector<CommandResultPayload>* results,
                                   int32_t* error) {
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) {
        *error = kBadDisplay;
        return false;
    }
    if (state->validation != ValidationState::kValidated) {
        *error = kNotValidated;
        return false;
    }
    ::android::base::unique_fd fence;
    const bool presented = drm_.Present(display, state->target, state->target_fence.get(),
                                        state->target_full_damage, state->target_damage, &fence);
    state->target_fence.reset();
    if (!presented) {
        *error = kNoResources;
        return false;
    }
    if (fence.ok()) {
        PresentFence present;
        present.display = display;
        present.fence = ndk::ScopedFileDescriptor(fence.release());
        results->emplace_back(std::move(present));
    }
    state->scanout = state->target;
    // Client-target damage describes one frame. If no new target is supplied,
    // full damage is safer than replaying stale partial rectangles.
    state->target_full_damage = true;
    state->target_damage.clear();
    return true;
}

ndk::ScopedAStatus ComposerClient::executeCommands(const std::vector<DisplayCommand>& commands,
                                                   std::vector<CommandResultPayload>* results) {
    std::lock_guard lock(mutex_);
    results->clear();
    for (size_t i = 0; i < commands.size(); ++i) {
        const size_t result_start = results->size();
        const DisplayCommand& command = commands[i];
        DisplayState* state = FindStateLocked(command.display);
        if (state == nullptr) {
            AddError(i, kBadDisplay, results);
            continue;
        }
        int32_t error = 0;
        bool valid = true;
        if (command.virtualDisplayOutputBuffer) {
            AddError(i, kUnsupported, results);
            continue;
        }
        if (command.brightness && (!std::isfinite(command.brightness->brightness) ||
                                   command.brightness->brightness > 1.0F)) {
            AddError(i, kBadParameter, results);
            continue;
        }
        if (command.brightness && !drm_.HasBrightness(command.display)) {
            AddError(i, kUnsupported, results);
            continue;
        }
        const auto saved_layers = state->layers;
        const auto saved_slots = state->target_slots;
        const auto saved_target = state->target;
        const bool saved_target_full_damage = state->target_full_damage;
        const auto saved_target_damage = state->target_damage;
        const int64_t saved_next_layer = state->next_layer;
        const ValidationState saved_validation = state->validation;
        ::android::base::unique_fd saved_target_fence(
                state->target_fence.ok() ? dup(state->target_fence.get()) : -1);
        if (state->target_fence.ok() && !saved_target_fence.ok()) {
            AddError(i, kNoResources, results);
            continue;
        }
        auto restore_state = [&] {
            state->layers = saved_layers;
            state->target_slots = saved_slots;
            state->target = saved_target;
            state->target_full_damage = saved_target_full_damage;
            state->target_damage = saved_target_damage;
            state->next_layer = saved_next_layer;
            state->target_fence = std::move(saved_target_fence);
            state->validation = saved_validation;
        };
        if (command.colorTransformMatrix && command.colorTransformMatrix->size() != 16) {
            AddError(i, kBadParameter, results);
            continue;
        }
        if (command.colorTransformMatrix) {
            state->validation = ValidationState::kDirty;
        }
        for (const LayerCommand& layer : command.layers) {
            if (!ApplyLayerLocked(command.display, layer, &error)) {
                valid = false;
                break;
            }
        }
        if (valid && command.clientTarget) {
            valid = SetClientTargetLocked(command.display, *command.clientTarget, &error);
        }
        if (!valid) {
            restore_state();
            AddError(i, error, results);
            continue;
        }
        if (command.validateDisplay || command.presentOrValidateDisplay) {
            ValidateLocked(command.display, results);
            const bool test_ok =
                    drm_.Test(command.display, state->target, state->target_fence.get());
            if (!test_ok) {
                results->resize(result_start);
                restore_state();
                AddError(i, kNoResources, results);
                continue;
            }
        }
        if (command.acceptDisplayChanges) {
            if (state->validation == ValidationState::kDirty) {
                results->resize(result_start);
                restore_state();
                AddError(i, kNotValidated, results);
                continue;
            }
            for (auto& item : state->layers) {
                item.second.composition = Composition::CLIENT;
            }
            state->validation = ValidationState::kValidated;
        }
        if (command.presentOrValidateDisplay) {
            PresentOrValidate result;
            result.display = command.display;
            result.result = PresentOrValidate::Result::Validated;
            results->emplace_back(result);
            // SKIP_VALIDATE is not advertised, so presentOrValidate always validates.
        }
        if (command.brightness &&
            !drm_.SetBrightness(command.display, command.brightness->brightness)) {
            results->resize(result_start);
            restore_state();
            AddError(i, kNoResources, results);
            continue;
        }
        if (command.presentDisplay) {
            if (command.expectedPresentTime && command.expectedPresentTime->timestampNanos > 0) {
                timespec deadline{
                        .tv_sec = command.expectedPresentTime->timestampNanos / 1000000000LL,
                        .tv_nsec = command.expectedPresentTime->timestampNanos % 1000000000LL};
                while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) ==
                       EINTR) {
                }
            }
            if (!PresentLocked(command.display, results, &error)) {
                results->resize(result_start);
                restore_state();
                AddError(i, error, results);
            }
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getActiveConfig(int64_t display, int32_t* config) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    *config = d->active_config;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getColorModes(int64_t display, std::vector<ColorMode>* modes) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    *modes = {ColorMode::NATIVE};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDataspaceSaturationMatrix(common::Dataspace dataspace,
                                                                std::vector<float>* matrix) {
    if (dataspace != common::Dataspace::SRGB_LINEAR) return Error(kBadParameter);
    *matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayAttribute(int64_t display, int32_t config,
                                                       DisplayAttribute attribute, int32_t* value) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    const drmfb::DrmConfig* c = FindConfig(*d, config);
    if (c == nullptr) return Error(kBadConfig);
    switch (attribute) {
        case DisplayAttribute::WIDTH:
            *value = c->mode.hdisplay;
            break;
        case DisplayAttribute::HEIGHT:
            *value = c->mode.vdisplay;
            break;
        case DisplayAttribute::VSYNC_PERIOD:
            *value = ModePeriod(c->mode);
            break;
        case DisplayAttribute::CONFIG_GROUP:
            *value = c->group;
            break;
        case DisplayAttribute::DPI_X:
            if (d->mm_width <= 0) return Error(kUnsupported);
            *value = static_cast<int32_t>(c->mode.hdisplay * 25400LL / d->mm_width);
            break;
        case DisplayAttribute::DPI_Y:
            if (d->mm_height <= 0) return Error(kUnsupported);
            *value = static_cast<int32_t>(c->mode.vdisplay * 25400LL / d->mm_height);
            break;
        default:
            return Error(kBadParameter);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayCapabilities(int64_t display,
                                                          std::vector<DisplayCapability>* caps) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    caps->clear();
    if (drm_.HasBrightness(display)) caps->push_back(DisplayCapability::BRIGHTNESS);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigs(int64_t display,
                                                     std::vector<int32_t>* configs) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    configs->clear();
    for (const auto& config : d->configs) configs->push_back(config.id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConnectionType(int64_t display,
                                                            DisplayConnectionType* type) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    *type = d->internal ? DisplayConnectionType::INTERNAL : DisplayConnectionType::EXTERNAL;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayIdentificationData(
        int64_t display, DisplayIdentification* identification) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    if (d->edid.empty()) return Error(kUnsupported);
    identification->port = static_cast<int8_t>(d->id & 0xff);
    identification->data = d->edid;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayName(int64_t display, std::string* name) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    *name = d->name;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayVsyncPeriod(int64_t display, int32_t* period) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    const auto* config = FindConfig(*d, d->active_config);
    if (config == nullptr) return Error(kBadConfig);
    *period = ModePeriod(config->mode);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayedContentSample(int64_t display, int64_t, int64_t,
                                                             DisplayContentSample*) {
    return UnsupportedDisplay(display);
}
ndk::ScopedAStatus ComposerClient::getDisplayedContentSamplingAttributes(
        int64_t display, DisplayContentSamplingAttributes*) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::getDisplayPhysicalOrientation(int64_t display,
                                                                 common::Transform* orientation) {
    std::lock_guard lock(mutex_);
    const drmfb::DrmDisplay* drm_display = FindDisplayLocked(display);
    if (drm_display == nullptr) return Error(kBadDisplay);
    switch (drm_display->orientation_degrees) {
        case 90:
            *orientation = common::Transform::ROT_90;
            break;
        case 180:
            *orientation = common::Transform::ROT_180;
            break;
        case 270:
            *orientation = common::Transform::ROT_270;
            break;
        default:
            *orientation = common::Transform::NONE;
            break;
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getHdrCapabilities(int64_t display, HdrCapabilities* caps) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    *caps = HdrCapabilities{};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getMaxVirtualDisplayCount(int32_t* count) {
    *count = 0;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getPerFrameMetadataKeys(int64_t display,
                                                           std::vector<PerFrameMetadataKey>*) {
    return UnsupportedDisplay(display);
}
ndk::ScopedAStatus ComposerClient::getReadbackBufferAttributes(int64_t display,
                                                               ReadbackBufferAttributes*) {
    return UnsupportedDisplay(display);
}
ndk::ScopedAStatus ComposerClient::getReadbackBufferFence(int64_t display,
                                                          ndk::ScopedFileDescriptor* fence) {
    *fence = ndk::ScopedFileDescriptor(-1);
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::getRenderIntents(int64_t display, ColorMode mode,
                                                    std::vector<RenderIntent>* intents) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    if (mode != ColorMode::NATIVE) return Error(kBadParameter);
    *intents = {RenderIntent::COLORIMETRIC};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getSupportedContentTypes(int64_t display,
                                                            std::vector<ContentType>* types) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    types->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayDecorationSupport(
        int64_t display, std::optional<common::DisplayDecorationSupport>*) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::registerCallback(
        const std::shared_ptr<IComposerCallback>& callback) {
    std::vector<int64_t> connected;
    std::unique_lock callback_lock(hotplug_callback_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (callback_ != nullptr || callback == nullptr) return Error(kBadParameter);
        callback_ = callback;
        for (const auto& [id, display] : drm_.displays()) {
            if (display.connected) connected.push_back(id);
        }
    }
    for (int64_t display : connected) {
        ndk::ScopedAStatus status =
                callback->onHotplugEvent(display, common::DisplayHotplugEvent::CONNECTED);
        if (!status.isOk()) {
            ALOGW("Initial hotplug callback failed: %s", status.getDescription().c_str());
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setActiveConfig(int64_t display, int32_t config) {
    bool notify_refresh = false;
    {
        std::lock_guard lock(mutex_);
        drmfb::DrmDisplay* drm_display = FindDisplayLocked(display);
        if (drm_display == nullptr) return Error(kBadDisplay);
        const drmfb::DrmConfig* requested_config = FindConfig(*drm_display, config);
        if (requested_config == nullptr) return Error(kBadConfig);
        if (drm_display->active_config == config) return ndk::ScopedAStatus::ok();
        if (!drm_.SetActiveConfig(display, config)) {
            return Error(IComposerClient::EX_CONFIG_FAILED);
        }
        states_[display].target_slots.assign(states_[display].target_slots.size(), nullptr);
        states_[display].target.reset();
        states_[display].target_full_damage = true;
        states_[display].target_damage.clear();
        states_[display].scanout.reset();
        states_[display].validation = ValidationState::kDirty;
        notify_refresh = states_[display].refresh_debug_enabled;
    }
    if (notify_refresh) NotifyRefreshRateChanged(display);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setActiveConfigWithConstraints(
        int64_t display, int32_t config, const VsyncPeriodChangeConstraints& constraints,
        VsyncPeriodChangeTimeline* timeline) {
    int32_t old_group = 0;
    int32_t new_group = 0;
    {
        std::lock_guard lock(mutex_);
        drmfb::DrmDisplay* drm_display = FindDisplayLocked(display);
        if (drm_display == nullptr) return Error(kBadDisplay);
        const drmfb::DrmConfig* old_config = FindConfig(*drm_display, drm_display->active_config);
        const drmfb::DrmConfig* new_config = FindConfig(*drm_display, config);
        if (new_config == nullptr) return Error(kBadConfig);
        old_group = old_config == nullptr ? -1 : old_config->group;
        new_group = new_config->group;
    }
    if (constraints.seamlessRequired) {
        return Error(old_group == new_group ? IComposerClient::EX_SEAMLESS_NOT_POSSIBLE
                                            : IComposerClient::EX_SEAMLESS_NOT_ALLOWED);
    }
    if (constraints.desiredTimeNanos > 0) {
        timespec deadline{.tv_sec = constraints.desiredTimeNanos / 1000000000LL,
                          .tv_nsec = constraints.desiredTimeNanos % 1000000000LL};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
        }
    }
    ndk::ScopedAStatus status = setActiveConfig(display, config);
    if (!status.isOk()) return status;
    timeline->newVsyncAppliedTimeNanos = MonotonicNanos();
    timeline->refreshTimeNanos = timeline->newVsyncAppliedTimeNanos;
    timeline->refreshRequired = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setBootDisplayConfig(int64_t display, int32_t) {
    return UnsupportedDisplay(display);
}
ndk::ScopedAStatus ComposerClient::clearBootDisplayConfig(int64_t display) {
    return UnsupportedDisplay(display);
}
ndk::ScopedAStatus ComposerClient::getPreferredBootDisplayConfig(int64_t display, int32_t*) {
    return UnsupportedDisplay(display);
}
ndk::ScopedAStatus ComposerClient::setAutoLowLatencyMode(int64_t display, bool) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::setClientTargetSlotCount(int64_t display, int32_t count) {
    if (count < 0) return Error(kBadParameter);
    if (count > kMaxBufferSlots) return Error(kNoResources);
    std::lock_guard lock(mutex_);
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) return Error(kBadDisplay);
    state->target.reset();
    state->target_slots.assign(count, nullptr);
    state->target_full_damage = true;
    state->target_damage.clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setColorMode(int64_t display, ColorMode mode,
                                                RenderIntent intent) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    if (mode != ColorMode::NATIVE) return Error(kBadParameter);
    if (intent < RenderIntent::COLORIMETRIC || intent > RenderIntent::TONE_MAP_ENHANCE) {
        return Error(kBadParameter);
    }
    if (intent != RenderIntent::COLORIMETRIC) return Error(kUnsupported);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setContentType(int64_t display, ContentType type) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    return type == ContentType::NONE ? ndk::ScopedAStatus::ok() : Error(kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setDisplayedContentSamplingEnabled(int64_t display, bool,
                                                                      FormatColorComponent,
                                                                      int64_t) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::setPowerMode(int64_t display, PowerMode mode) {
    {
        std::lock_guard lock(mutex_);
        if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    }
    bool on;
    switch (mode) {
        case PowerMode::OFF:
            on = false;
            break;
        case PowerMode::ON:
            on = true;
            break;
        case PowerMode::DOZE:
        case PowerMode::DOZE_SUSPEND:
        case PowerMode::ON_SUSPEND:
            return Error(kUnsupported);
        default:
            return Error(kBadParameter);
    }
    std::lock_guard lock(mutex_);
    DisplayState* state = FindStateLocked(display);
    if (state == nullptr) return Error(kBadDisplay);
    if (!drm_.SetPower(display, on)) return Error(kNoResources);
    if (!on) {
        state->vsync_enabled = false;
        state->scanout.reset();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setReadbackBuffer(
        int64_t display, const ::aidl::android::hardware::common::NativeHandle&,
        const ndk::ScopedFileDescriptor&) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::setVsyncEnabled(int64_t display, bool enabled) {
    {
        std::lock_guard lock(mutex_);
        DisplayState* state = FindStateLocked(display);
        if (state == nullptr) return Error(kBadDisplay);
        state->vsync_enabled = enabled;
    }
    vsync_cv_.notify_all();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setIdleTimerEnabled(int64_t display, int32_t timeout_ms) {
    std::lock_guard lock(mutex_);
    if (FindDisplayLocked(display) == nullptr) return Error(kBadDisplay);
    return timeout_ms < 0 ? Error(kBadParameter) : Error(kUnsupported);
}
ndk::ScopedAStatus ComposerClient::getOverlaySupport(OverlayProperties*) {
    return Error(kUnsupported);
}
ndk::ScopedAStatus ComposerClient::getHdrConversionCapabilities(
        std::vector<common::HdrConversionCapability>*) {
    return Error(kUnsupported);
}
ndk::ScopedAStatus ComposerClient::setHdrConversionStrategy(const common::HdrConversionStrategy&,
                                                            common::Hdr*) {
    return Error(kUnsupported);
}
ndk::ScopedAStatus ComposerClient::setRefreshRateChangedCallbackDebugEnabled(int64_t display,
                                                                             bool enabled) {
    std::unique_lock refresh_lock(refresh_callback_mutex_);
    std::shared_ptr<IComposerCallback> callback;
    int32_t period = 0;
    {
        std::lock_guard lock(mutex_);
        DisplayState* state = FindStateLocked(display);
        drmfb::DrmDisplay* drm_display = FindDisplayLocked(display);
        if (state == nullptr || drm_display == nullptr) return Error(kBadDisplay);
        const drmfb::DrmConfig* config = FindConfig(*drm_display, drm_display->active_config);
        if (config == nullptr) return Error(kBadConfig);
        state->refresh_debug_enabled = enabled;
        period = ModePeriod(config->mode);
        callback = callback_;
    }
    if (enabled && callback != nullptr) {
        RefreshRateChangedDebugData data;
        data.display = display;
        data.vsyncPeriodNanos = period;
        data.refreshPeriodNanos = period;
        ndk::ScopedAStatus status = callback->onRefreshRateChangedDebug(data);
        if (!status.isOk()) {
            ALOGW("Refresh-rate debug callback failed: %s", status.getDescription().c_str());
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigurations(
        int64_t display, int32_t, std::vector<DisplayConfiguration>* configurations) {
    std::lock_guard lock(mutex_);
    auto* d = FindDisplayLocked(display);
    if (d == nullptr) return Error(kBadDisplay);
    configurations->clear();
    for (const auto& config : d->configs) {
        configurations->push_back(ToAidlConfig(*d, config));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::notifyExpectedPresent(int64_t display,
                                                         const ClockMonotonicTimestamp&, int32_t) {
    std::lock_guard lock(mutex_);
    return FindDisplayLocked(display) == nullptr ? Error(kBadDisplay) : ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::startHdcpNegotiation(int64_t display, const drm::HdcpLevels&) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::getMaxLayerPictureProfiles(int64_t display, int32_t*) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::getLuts(int64_t display, const std::vector<Buffer>&,
                                           std::vector<Luts>*) {
    return UnsupportedDisplay(display);
}

int64_t ComposerClient::MonotonicNanos() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

void ComposerClient::NotifyHotplug(int64_t display, bool connected) {
    std::shared_ptr<IComposerCallback> callback;
    {
        std::lock_guard lock(mutex_);
        callback = callback_;
    }
    if (callback == nullptr) return;
    std::lock_guard callback_lock(hotplug_callback_mutex_);
    const auto event = connected ? common::DisplayHotplugEvent::CONNECTED
                                 : common::DisplayHotplugEvent::DISCONNECTED;
    ndk::ScopedAStatus status = callback->onHotplugEvent(display, event);
    if (!status.isOk()) {
        ALOGW("Hotplug callback failed: %s", status.getDescription().c_str());
    }
}

void ComposerClient::NotifyRefreshRateChanged(int64_t display) {
    std::unique_lock refresh_lock(refresh_callback_mutex_);
    std::shared_ptr<IComposerCallback> callback;
    int32_t period = 0;
    {
        std::lock_guard lock(mutex_);
        auto state = states_.find(display);
        if (state == states_.end() || !state->second.refresh_debug_enabled) return;
        drmfb::DrmDisplay* drm_display = FindDisplayLocked(display);
        if (drm_display == nullptr) return;
        const drmfb::DrmConfig* config = FindConfig(*drm_display, drm_display->active_config);
        if (config == nullptr) return;
        period = ModePeriod(config->mode);
        callback = callback_;
    }
    if (callback == nullptr) return;
    RefreshRateChangedDebugData data;
    data.display = display;
    data.vsyncPeriodNanos = period;
    data.refreshPeriodNanos = period;
    ndk::ScopedAStatus status = callback->onRefreshRateChangedDebug(data);
    if (!status.isOk()) {
        ALOGW("Refresh-rate debug callback failed: %s", status.getDescription().c_str());
    }
}

void ComposerClient::HotplugLoop() {
    pollfd fds[2] = {{hotplug_socket_.get(), POLLIN, 0}, {wake_fd_.get(), POLLIN, 0}};
    while (!stopping_) {
        const int count = poll(fds, 2, -1);
        if (count <= 0) continue;
        if ((fds[1].revents & POLLIN) != 0) break;
        if ((fds[0].revents & POLLIN) == 0) continue;
        char message[4096];
        const ssize_t length = recv(hotplug_socket_.get(), message, sizeof(message) - 1, 0);
        if (length <= 0) continue;
        message[length] = '\0';
        bool drm_event = false;
        bool hotplug = false;
        for (size_t offset = 0; offset < static_cast<size_t>(length);) {
            const char* field = message + offset;
            drm_event |= strcmp(field, "DEVTYPE=drm_minor") == 0;
            hotplug |= strcmp(field, "HOTPLUG=1") == 0;
            offset += strlen(field) + 1;
        }
        if (!drm_event || !hotplug) continue;
        usleep(200000);
        std::vector<drmfb::DrmDevice::HotplugChange> changes;
        {
            std::lock_guard lock(mutex_);
            changes = drm_.Rescan();
            for (const auto& change : changes) {
                if (change.connected) {
                    states_.try_emplace(change.display);
                    drmfb::DrmDisplay* display = FindDisplayLocked(change.display);
                    if (display != nullptr && display->powered &&
                        !drm_.SetPower(change.display, false)) {
                        ALOGW("Failed to establish OFF state for hotplugged display %" PRId64,
                              change.display);
                    }
                } else {
                    states_.erase(change.display);
                }
            }
        }
        for (const auto& change : changes) {
            ALOGI("Display %" PRId64 " hotplug connected=%d", change.display, change.connected);
            NotifyHotplug(change.display, change.connected);
        }
    }
    ALOGI("Hotplug worker stopped");
}

void ComposerClient::VblankLoop() {
    int64_t previous_display = -1;
    while (!stopping_) {
        int64_t display_id = -1;
        uint32_t crtc_id = 0;
        int32_t period = 16666666;
        std::shared_ptr<IComposerCallback> callback;
        {
            std::unique_lock lock(mutex_);
            vsync_cv_.wait(lock, [this] {
                if (stopping_) return true;
                return std::any_of(states_.begin(), states_.end(),
                                   [](const auto& item) { return item.second.vsync_enabled; });
            });
            if (stopping_) break;
            auto select = [this](auto begin, auto end, int64_t after) {
                for (auto it = begin; it != end; ++it) {
                    auto display = drm_.displays().find(it->first);
                    if (it->first > after && it->second.vsync_enabled &&
                        display != drm_.displays().end() && display->second.connected) {
                        return it;
                    }
                }
                return end;
            };
            auto selected = select(states_.begin(), states_.end(), previous_display);
            if (selected == states_.end()) {
                selected = select(states_.begin(), states_.end(), -1);
            }
            if (selected != states_.end()) {
                auto display = drm_.displays().find(selected->first);
                display_id = selected->first;
                previous_display = display_id;
                crtc_id = display->second.crtc_id;
                const auto* config = FindConfig(display->second, display->second.active_config);
                if (config != nullptr) period = ModePeriod(config->mode);
            }
        }
        if (display_id < 0) continue;
        int64_t timestamp = 0;
        SequenceResult sequence_result;
        uint64_t queued_sequence = 0;
        int queue_result;
        {
            std::unique_lock event_lock(drm_.event_mutex());
            queue_result = drmCrtcQueueSequence(
                    drm_.fd(), crtc_id, DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS,
                    1, &queued_sequence, reinterpret_cast<uint64_t>(&sequence_result));
            if (queue_result == 0) {
                pollfd fds[2] = {{drm_.fd(), POLLIN, 0}, {wake_fd_.get(), POLLIN, 0}};
                while (!stopping_ && sequence_result.timestamp_ns == 0) {
                    const int count = poll(fds, 2, -1);
                    if (count < 0 && errno == EINTR) continue;
                    if (count <= 0) continue;
                    if ((fds[1].revents & POLLIN) != 0) break;
                    if ((fds[0].revents & POLLIN) != 0) {
                        drmEventContext context{};
                        context.version = DRM_EVENT_CONTEXT_VERSION;
                        context.sequence_handler = HandleSequence;
                        if (drmHandleEvent(drm_.fd(), &context) != 0) {
                            ALOGW("Failed to handle CRTC sequence event: %s", strerror(errno));
                        }
                    }
                }
            }
        }
        if (stopping_) break;
        if (queue_result == 0) {
            timestamp = static_cast<int64_t>(sequence_result.timestamp_ns);
        } else {
            std::unique_lock lock(mutex_);
            vsync_cv_.wait_for(lock, std::chrono::nanoseconds(period),
                               [this] { return stopping_.load(); });
            if (stopping_) break;
            timestamp = MonotonicNanos();
        }
        {
            std::lock_guard lock(mutex_);
            auto state = states_.find(display_id);
            if (state == states_.end() || !state->second.vsync_enabled ||
                FindDisplayLocked(display_id) == nullptr) {
                continue;
            }
            callback = callback_;
        }
        if (callback != nullptr && !stopping_) {
            ndk::ScopedAStatus status = callback->onVsync(display_id, timestamp, period);
            if (!status.isOk()) {
                ALOGW("Vsync callback failed: %s", status.getDescription().c_str());
            }
        }
    }
    ALOGI("Vblank worker stopped");
}

std::string ComposerClient::Dump() {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << "drmfb Composer3 V4\n" << drm_.Dump();
    for (const auto& [id, state] : states_) {
        out << " state display=" << id << " layers=" << state.layers.size()
            << " targetSlots=" << state.target_slots.size()
            << " target=" << (state.target ? state.target->id() : 0)
            << " validation=" << static_cast<int>(state.validation)
            << " vsync=" << state.vsync_enabled << '\n';
    }
    return out.str();
}

ndk::SpAIBinder ComposerClient::createBinder() {
    auto binder = BnComposerClient::createBinder();
    AIBinder_setInheritRt(binder.get(), true);
    return binder;
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
