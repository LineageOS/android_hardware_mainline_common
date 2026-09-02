/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-composer3"

#include "ComposerClient.h"

#include <aidl/android/hardware/graphics/common/DisplayHotplugEvent.h>
#include <aidl/android/hardware/graphics/composer3/ChangedCompositionTypes.h>
#include <aidl/android/hardware/graphics/composer3/CommandError.h>
#include <aidl/android/hardware/graphics/composer3/PresentFence.h>
#include <aidl/android/hardware/graphics/composer3/PresentOrValidate.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android/binder_ibinder_platform.h>
#include <cutils/native_handle.h>
#include <log/log.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>

namespace aidl::android::hardware::graphics::composer3::impl {
namespace {

namespace fs = std::filesystem;

constexpr int32_t kConfig = 0;
constexpr int32_t kBadConfig = IComposerClient::EX_BAD_CONFIG;
constexpr int32_t kBadDisplay = IComposerClient::EX_BAD_DISPLAY;
constexpr int32_t kBadLayer = IComposerClient::EX_BAD_LAYER;
constexpr int32_t kBadParameter = IComposerClient::EX_BAD_PARAMETER;
constexpr int32_t kNoResources = IComposerClient::EX_NO_RESOURCES;
constexpr int32_t kNotValidated = IComposerClient::EX_NOT_VALIDATED;
constexpr int32_t kUnsupported = IComposerClient::EX_UNSUPPORTED;
constexpr int32_t kMaxBufferSlots = 4096;
constexpr int32_t kMaxFramebufferDevices = 32;

}  // namespace

ComposerClient::ComposerClient(std::string path) : path_(std::move(path)) {}

ComposerClient::~ComposerClient() {
    stopping_ = true;
    for (const auto& display : displays_) display->device->InterruptVsyncWait();
    vsync_cv_.notify_all();
    for (const auto& display : displays_) {
        if (display->vsync_thread.joinable()) display->vsync_thread.join();
    }
    std::lock_guard lock(mutex_);
    callback_.reset();
    for (const auto& display : displays_) {
        display->state.target.reset();
        display->state.target_damage.clear();
        display->state.target_slots.clear();
    }
    ALOGI("Composer client stopped");
}

bool ComposerClient::Init() {
    std::vector<std::string> candidates;
    if (!path_.empty()) {
        for (const std::string& path : ::android::base::Split(path_, ",")) {
            const std::string trimmed = ::android::base::Trim(path);
            if (!trimmed.empty()) candidates.push_back(trimmed);
        }
    } else {
        for (int32_t index = 0; index < kMaxFramebufferDevices; ++index) {
            candidates.push_back("/dev/graphics/fb" + std::to_string(index));
            candidates.push_back("/dev/fb" + std::to_string(index));
        }
    }
    std::set<dev_t> devices;
    for (const std::string& path : candidates) {
        struct stat status{};
        if (stat(path.c_str(), &status) != 0 || !S_ISCHR(status.st_mode) ||
            devices.count(status.st_rdev) != 0) {
            continue;
        }
        auto context = std::make_unique<DisplayContext>();
        context->device = std::make_unique<fb::FbdevDevice>();
        if (context->device->Init(path)) {
            devices.insert(status.st_rdev);
            displays_.push_back(std::move(context));
        }
    }
    if (displays_.empty()) {
        ALOGE("No usable fbdev displays found");
        return false;
    }
    if (displays_.size() == 1) {
        std::error_code error;
        fs::directory_iterator iterator("/sys/class/backlight", error);
        fs::directory_iterator end;
        std::vector<std::string> backlights;
        while (!error && iterator != end) {
            const std::string backlight = iterator->path().string();
            if (fs::exists(backlight + "/brightness", error) && !error &&
                fs::exists(backlight + "/max_brightness", error) && !error &&
                fs::exists(backlight + "/bl_power", error) && !error) {
                backlights.push_back(backlight);
            }
            iterator.increment(error);
        }
        if (!error && backlights.size() == 1) {
            std::string maximum;
            uint32_t max_value = 0;
            if (::android::base::ReadFileToString(backlights[0] + "/max_brightness", &maximum) &&
                ::android::base::ParseUint(::android::base::Trim(maximum), &max_value) &&
                max_value != 0) {
                displays_[0]->device->SetBacklight(backlights[0], max_value);
            }
        }
    }
    for (size_t display = 0; display < displays_.size(); ++display) {
        displays_[display]->vsync_thread =
                std::thread(&ComposerClient::VsyncLoop, this, static_cast<int64_t>(display));
    }
    return true;
}

ndk::ScopedAStatus ComposerClient::Error(int32_t error) {
    return ndk::ScopedAStatus::fromServiceSpecificError(error);
}

bool ComposerClient::IsDisplay(int64_t display) const {
    return display >= 0 && static_cast<size_t>(display) < displays_.size();
}

ComposerClient::DisplayContext* ComposerClient::GetDisplay(int64_t display) {
    return IsDisplay(display) ? displays_[display].get() : nullptr;
}

const ComposerClient::DisplayContext* ComposerClient::GetDisplay(int64_t display) const {
    return IsDisplay(display) ? displays_[display].get() : nullptr;
}

ndk::ScopedAStatus ComposerClient::UnsupportedDisplay(int64_t display) {
    return IsDisplay(display) ? Error(kUnsupported) : Error(kBadDisplay);
}

ndk::ScopedAStatus ComposerClient::createLayer(int64_t display, int32_t slots, int64_t* layer) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    if (slots < 0) return Error(kBadParameter);
    if (slots > kMaxBufferSlots) return Error(kNoResources);
    std::lock_guard lock(mutex_);
    DisplayState& state = GetDisplay(display)->state;
    if (state.next_layer == std::numeric_limits<int64_t>::max()) return Error(kNoResources);
    *layer = state.next_layer;
    ++state.next_layer;
    state.layers.emplace(*layer, LayerState{.slots = slots});
    state.validation = ValidationState::kDirty;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::createVirtualDisplay(int32_t, int32_t, common::PixelFormat,
                                                        int32_t, VirtualDisplay*) {
    return Error(kUnsupported);
}

ndk::ScopedAStatus ComposerClient::destroyLayer(int64_t display, int64_t layer) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    std::lock_guard lock(mutex_);
    DisplayState& state = GetDisplay(display)->state;
    if (state.layers.erase(layer) == 0) return Error(kBadLayer);
    if (state.layers.empty()) {
        state.target.reset();
        state.target_fence.reset();
    }
    state.validation = ValidationState::kDirty;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::destroyVirtualDisplay(int64_t display) {
    return IsDisplay(display) ? Error(kBadParameter) : Error(kBadDisplay);
}

void ComposerClient::AddError(size_t index, int32_t error,
                              std::vector<CommandResultPayload>* results) {
    results->emplace_back(
            CommandError{.commandIndex = static_cast<int32_t>(index), .errorCode = error});
}

bool ComposerClient::ApplyLayerLocked(DisplayState* state, const LayerCommand& command,
                                      int32_t* error) {
    switch (command.layerLifecycleBatchCommandType) {
        case LayerLifecycleBatchCommandType::CREATE:
            if (command.newBufferSlotCount < 0) {
                *error = kBadParameter;
                return false;
            }
            if (command.newBufferSlotCount > kMaxBufferSlots) {
                *error = kNoResources;
                return false;
            }
            if (state->layers.count(command.layer) != 0) {
                *error = kBadLayer;
                return false;
            }
            state->layers.emplace(command.layer, LayerState{.slots = command.newBufferSlotCount});
            if (command.layer >= state->next_layer &&
                command.layer != std::numeric_limits<int64_t>::max()) {
                state->next_layer = command.layer + 1;
            }
            state->validation = ValidationState::kDirty;
            break;
        case LayerLifecycleBatchCommandType::DESTROY:
            if (state->layers.erase(command.layer) == 0) {
                *error = kBadLayer;
                return false;
            }
            if (state->layers.empty()) {
                state->target.reset();
                state->target_fence.reset();
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
    const Composition requested =
            command.composition ? command.composition->composition : layer->second.composition;
    if (command.buffer && requested != Composition::CLIENT &&
        requested != Composition::SOLID_COLOR &&
        (command.buffer->slot < 0 || command.buffer->slot >= layer->second.slots)) {
        *error = kBadParameter;
        return false;
    }
    if (command.bufferSlotsToClear) {
        for (int32_t slot : *command.bufferSlotsToClear) {
            if (slot < 0 || slot >= layer->second.slots) {
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
    const bool color_update = command.color && requested == Composition::SOLID_COLOR;
    const bool state_update = command.cursorPosition || command.blendMode || color_update ||
                              command.composition || command.dataspace || command.displayFrame ||
                              command.planeAlpha || command.sourceCrop || command.transform ||
                              command.visibleRegion || command.z || command.colorTransform ||
                              command.brightness || command.perFrameMetadata ||
                              command.perFrameMetadataBlob || command.blockingRegion;
    if (state_update) state->validation = ValidationState::kDirty;
    return true;
}

bool ComposerClient::SetClientTargetLocked(DisplayContext* display, const ClientTarget& target,
                                           int32_t* error) {
    DisplayState* state = &display->state;
    fb::FbdevDevice* device = display->device.get();
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
        auto imported = device->ImportBuffer(raw);
        native_handle_delete(raw);
        if (imported == nullptr) {
            *error = kBadParameter;
            return false;
        }
        state->target_slots[buffer.slot] = std::move(imported);
    }
    state->target = state->target_slots[buffer.slot];
    if (state->target == nullptr && buffer.handle) {
        *error = kBadParameter;
        return false;
    }
    state->target_damage.clear();
    if (target.damage.empty()) {
        state->target_damage.push_back({0, 0, device->width(), device->height()});
    } else if (!(target.damage.size() == 1 && target.damage[0].left == target.damage[0].right &&
                 target.damage[0].top == target.damage[0].bottom)) {
        for (const common::Rect& rect : target.damage) {
            if (rect.left > rect.right || rect.top > rect.bottom) {
                *error = kBadParameter;
                return false;
            }
            const int32_t left = std::clamp(rect.left, 0, static_cast<int32_t>(device->width()));
            const int32_t top = std::clamp(rect.top, 0, static_cast<int32_t>(device->height()));
            const int32_t right = std::clamp(rect.right, 0, static_cast<int32_t>(device->width()));
            const int32_t bottom =
                    std::clamp(rect.bottom, 0, static_cast<int32_t>(device->height()));
            if (left < right && top < bottom) {
                state->target_damage.push_back(
                        {static_cast<uint32_t>(left), static_cast<uint32_t>(top),
                         static_cast<uint32_t>(right), static_cast<uint32_t>(bottom)});
            }
        }
    }
    if (buffer.fence.get() >= 0) {
        state->target_fence.reset(dup(buffer.fence.get()));
        if (!state->target_fence.ok()) {
            *error = kNoResources;
            return false;
        }
    } else {
        state->target_fence.reset();
    }
    return true;
}

void ComposerClient::ValidateLocked(int64_t display, DisplayState* state,
                                    std::vector<CommandResultPayload>* results) {
    ChangedCompositionTypes changes;
    changes.display = display;
    for (const auto& [id, layer] : state->layers) {
        if (layer.composition != Composition::CLIENT) {
            changes.layers.push_back({.layer = id, .composition = Composition::CLIENT});
        }
    }
    state->validation =
            changes.layers.empty() ? ValidationState::kValidated : ValidationState::kAwaitingAccept;
    if (!changes.layers.empty()) results->emplace_back(std::move(changes));
}

bool ComposerClient::PresentLocked(int64_t display, DisplayContext* context,
                                   std::vector<CommandResultPayload>* results, int32_t* error) {
    DisplayState* state = &context->state;
    if (state->validation != ValidationState::kValidated) {
        *error = kNotValidated;
        return false;
    }
    if (!state->layers.empty() && state->target == nullptr) {
        *error = kBadParameter;
        return false;
    }
    ::android::base::unique_fd fence;
    const bool presented = context->device->Present(state->target, state->target_damage,
                                                    state->target_fence.get(), &fence);
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
    return true;
}

ndk::ScopedAStatus ComposerClient::executeCommands(const std::vector<DisplayCommand>& commands,
                                                   std::vector<CommandResultPayload>* results) {
    std::lock_guard lock(mutex_);
    results->clear();
    for (size_t i = 0; i < commands.size(); ++i) {
        const size_t result_start = results->size();
        const DisplayCommand& command = commands[i];
        if (!IsDisplay(command.display)) {
            AddError(i, kBadDisplay, results);
            continue;
        }
        if (command.virtualDisplayOutputBuffer) {
            AddError(i, kUnsupported, results);
            continue;
        }
        DisplayContext* context = GetDisplay(command.display);
        if (command.brightness && (!std::isfinite(command.brightness->brightness) ||
                                   command.brightness->brightness > 1.0F)) {
            AddError(i, kBadParameter, results);
            continue;
        }
        if (command.brightness && !context->device->HasBrightness()) {
            AddError(i, kUnsupported, results);
            continue;
        }
        // The single fbdev mode is always active, so applying it is a no-op that cannot
        // produce a visual artifact and is therefore always seamless.
        if (command.activeConfig && command.activeConfig->configId != kConfig) {
            AddError(i, kBadConfig, results);
            continue;
        }
        DisplayState& state = context->state;
        const auto saved_layers = state.layers;
        const auto saved_slots = state.target_slots;
        const auto saved_target = state.target;
        const auto saved_damage = state.target_damage;
        const int64_t saved_next_layer = state.next_layer;
        const ValidationState saved_validation = state.validation;
        ::android::base::unique_fd saved_fence(
                state.target_fence.ok() ? dup(state.target_fence.get()) : -1);
        if (state.target_fence.ok() && !saved_fence.ok()) {
            AddError(i, kNoResources, results);
            continue;
        }
        auto restore = [&] {
            state.layers = saved_layers;
            state.target_slots = saved_slots;
            state.target = saved_target;
            state.target_damage = saved_damage;
            state.next_layer = saved_next_layer;
            state.validation = saved_validation;
            state.target_fence = std::move(saved_fence);
        };
        int32_t error = 0;
        bool valid = true;
        if (command.colorTransformMatrix && command.colorTransformMatrix->size() != 16) {
            AddError(i, kBadParameter, results);
            continue;
        }
        if (command.colorTransformMatrix) state.validation = ValidationState::kDirty;
        for (const LayerCommand& layer : command.layers) {
            if (!ApplyLayerLocked(&state, layer, &error)) {
                valid = false;
                break;
            }
        }
        if (valid && command.clientTarget) {
            valid = SetClientTargetLocked(context, *command.clientTarget, &error);
        }
        if (!valid) {
            restore();
            AddError(i, error, results);
            continue;
        }
        if (command.validateDisplay || command.presentOrValidateDisplay) {
            ValidateLocked(command.display, &state, results);
            if (!context->device->Test(state.target)) {
                results->resize(result_start);
                restore();
                AddError(i, kNoResources, results);
                continue;
            }
        }
        if (command.acceptDisplayChanges) {
            if (state.validation == ValidationState::kDirty) {
                results->resize(result_start);
                restore();
                AddError(i, kNotValidated, results);
                continue;
            }
            for (auto& item : state.layers) item.second.composition = Composition::CLIENT;
            state.validation = ValidationState::kValidated;
        }
        if (command.presentOrValidateDisplay) {
            results->emplace_back(PresentOrValidate{
                    .display = command.display, .result = PresentOrValidate::Result::Validated});
        }
        if (command.brightness && !context->device->SetBrightness(command.brightness->brightness)) {
            results->resize(result_start);
            restore();
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
            if (!PresentLocked(command.display, context, results, &error)) {
                results->resize(result_start);
                restore();
                AddError(i, error, results);
            }
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getActiveConfig(int64_t display, int32_t* config) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    *config = kConfig;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getColorModes(int64_t display, std::vector<ColorMode>* modes) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
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
    if (!IsDisplay(display)) return Error(kBadDisplay);
    if (config != kConfig) return Error(kBadConfig);
    const fb::FbdevDevice* device = GetDisplay(display)->device.get();
    switch (attribute) {
        case DisplayAttribute::WIDTH:
            *value = device->width();
            break;
        case DisplayAttribute::HEIGHT:
            *value = device->height();
            break;
        case DisplayAttribute::VSYNC_PERIOD:
            *value = device->period_ns();
            break;
        case DisplayAttribute::DPI_X:
            *value = static_cast<int32_t>(device->xdpi() * 1000);
            break;
        case DisplayAttribute::DPI_Y:
            *value = static_cast<int32_t>(device->ydpi() * 1000);
            break;
        case DisplayAttribute::CONFIG_GROUP:
            *value = 0;
            break;
        default:
            return Error(kBadParameter);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayCapabilities(int64_t display,
                                                          std::vector<DisplayCapability>* caps) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    caps->clear();
    if (GetDisplay(display)->device->HasBrightness()) {
        caps->push_back(DisplayCapability::BRIGHTNESS);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigs(int64_t display,
                                                     std::vector<int32_t>* configs) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    *configs = {kConfig};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConnectionType(int64_t display,
                                                            DisplayConnectionType* type) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    *type = DisplayConnectionType::INTERNAL;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayIdentificationData(int64_t display,
                                                                DisplayIdentification*) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::getDisplayName(int64_t display, std::string* name) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    *name = "fbdev-" + std::to_string(display) + " (" + GetDisplay(display)->device->path() + ")";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayVsyncPeriod(int64_t display, int32_t* period) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    *period = GetDisplay(display)->device->period_ns();
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
    if (!IsDisplay(display)) return Error(kBadDisplay);
    switch (GetDisplay(display)->device->rotation()) {
        case FB_ROTATE_UR:
            *orientation = common::Transform::NONE;
            break;
        case FB_ROTATE_CW:
            *orientation = common::Transform::ROT_90;
            break;
        case FB_ROTATE_UD:
            *orientation = common::Transform::ROT_180;
            break;
        case FB_ROTATE_CCW:
            *orientation = common::Transform::ROT_270;
            break;
        default:
            return Error(kNoResources);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getHdrCapabilities(int64_t display, HdrCapabilities* caps) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    *caps = {};
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
    if (!IsDisplay(display)) return Error(kBadDisplay);
    if (mode != ColorMode::NATIVE) return Error(kBadParameter);
    *intents = {RenderIntent::COLORIMETRIC};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getSupportedContentTypes(int64_t display,
                                                            std::vector<ContentType>* types) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    types->clear();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayDecorationSupport(
        int64_t display, std::optional<common::DisplayDecorationSupport>*) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::registerCallback(
        const std::shared_ptr<IComposerCallback>& callback) {
    if (callback == nullptr) return Error(kBadParameter);
    std::lock_guard callback_lock(callback_mutex_);
    {
        std::lock_guard lock(mutex_);
        if (callback_ != nullptr) return Error(kBadParameter);
        callback_ = callback;
    }
    for (size_t display = 0; display < displays_.size(); ++display) {
        const ndk::ScopedAStatus status = callback->onHotplugEvent(
                static_cast<int64_t>(display), common::DisplayHotplugEvent::CONNECTED);
        if (!status.isOk()) {
            ALOGW("Initial hotplug callback failed for display %zu: %s", display,
                  status.getDescription().c_str());
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setActiveConfig(int64_t display, int32_t config) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    return config == kConfig ? ndk::ScopedAStatus::ok() : Error(kBadConfig);
}

ndk::ScopedAStatus ComposerClient::setActiveConfigWithConstraints(
        int64_t display, int32_t config, const VsyncPeriodChangeConstraints& constraints,
        VsyncPeriodChangeTimeline* timeline) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    if (config != kConfig) return Error(kBadConfig);
    if (constraints.seamlessRequired) return Error(IComposerClient::EX_SEAMLESS_NOT_POSSIBLE);
    if (constraints.desiredTimeNanos > 0) {
        timespec deadline{.tv_sec = constraints.desiredTimeNanos / 1000000000LL,
                          .tv_nsec = constraints.desiredTimeNanos % 1000000000LL};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
        }
    }
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
    if (!IsDisplay(display)) return Error(kBadDisplay);
    if (count < 0) return Error(kBadParameter);
    if (count > kMaxBufferSlots) return Error(kNoResources);
    std::lock_guard lock(mutex_);
    DisplayState& state = GetDisplay(display)->state;
    state.target.reset();
    state.target_damage.clear();
    state.target_slots.assign(count, nullptr);
    state.target_fence.reset();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setColorMode(int64_t display, ColorMode mode,
                                                RenderIntent intent) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    if (mode != ColorMode::NATIVE) return Error(kBadParameter);
    if (intent < RenderIntent::COLORIMETRIC || intent > RenderIntent::TONE_MAP_ENHANCE) {
        return Error(kBadParameter);
    }
    return intent == RenderIntent::COLORIMETRIC ? ndk::ScopedAStatus::ok() : Error(kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setContentType(int64_t display, ContentType type) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    return type == ContentType::NONE ? ndk::ScopedAStatus::ok() : Error(kUnsupported);
}

ndk::ScopedAStatus ComposerClient::setDisplayedContentSamplingEnabled(int64_t display, bool,
                                                                      FormatColorComponent,
                                                                      int64_t) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::setPowerMode(int64_t display, PowerMode mode) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
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
    std::lock_guard callback_lock(vsync_callback_mutex_);
    std::lock_guard lock(mutex_);
    DisplayContext* context = GetDisplay(display);
    if (!context->device->SetPower(on)) return Error(kNoResources);
    if (!on) {
        context->state.vsync_enabled = false;
        // Vsync stops while the display is off, so the recorded sample stops being recent.
        context->state.have_vsync_sample = false;
        context->state.last_vsync_ns = 0;
    }
    vsync_cv_.notify_all();
    context->device->InterruptVsyncWait();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setReadbackBuffer(
        int64_t display, const ::aidl::android::hardware::common::NativeHandle&,
        const ndk::ScopedFileDescriptor&) {
    return UnsupportedDisplay(display);
}

ndk::ScopedAStatus ComposerClient::setVsyncEnabled(int64_t display, bool enabled) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    {
        std::lock_guard callback_lock(vsync_callback_mutex_);
        std::lock_guard lock(mutex_);
        GetDisplay(display)->state.vsync_enabled = enabled;
    }
    vsync_cv_.notify_all();
    GetDisplay(display)->device->InterruptVsyncWait();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::setIdleTimerEnabled(int64_t display, int32_t timeout_ms) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
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
    if (!IsDisplay(display)) return Error(kBadDisplay);
    std::unique_lock refresh_lock(refresh_callback_mutex_);
    std::shared_ptr<IComposerCallback> callback;
    const fb::FbdevDevice* device = GetDisplay(display)->device.get();
    {
        std::lock_guard lock(mutex_);
        GetDisplay(display)->state.refresh_debug_enabled = enabled;
        callback = callback_;
    }
    if (enabled && callback != nullptr) {
        RefreshRateChangedDebugData data{.display = display,
                                         .vsyncPeriodNanos = device->period_ns(),
                                         .refreshPeriodNanos = device->period_ns()};
        const auto status = callback->onRefreshRateChangedDebug(data);
        if (!status.isOk())
            ALOGW("Refresh debug callback failed: %s", status.getDescription().c_str());
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::getDisplayConfigurations(
        int64_t display, int32_t, std::vector<DisplayConfiguration>* configurations) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    const fb::FbdevDevice* device = GetDisplay(display)->device.get();
    DisplayConfiguration config;
    config.configId = kConfig;
    config.width = device->width();
    config.height = device->height();
    config.configGroup = 0;
    config.vsyncPeriod = device->period_ns();
    config.hdrOutputType = OutputType::SDR;
    config.dpi = DisplayConfiguration::Dpi{.x = device->xdpi(), .y = device->ydpi()};
    *configurations = {config};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ComposerClient::notifyExpectedPresent(int64_t display,
                                                         const ClockMonotonicTimestamp&,
                                                         int32_t frame_interval_ns) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    return frame_interval_ns < 0 ? Error(kBadParameter) : ndk::ScopedAStatus::ok();
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

ndk::ScopedAStatus ComposerClient::getDisplayKnownVsyncSample(int64_t display,
                                                              VsyncSample* sample) {
    if (!IsDisplay(display)) return Error(kBadDisplay);
    std::lock_guard lock(mutex_);
    const DisplayContext* context = GetDisplay(display);
    // Only hardware vsync events are reported. A software vsync cadence carries no
    // information about the display's real vsync phase.
    if (!context->state.have_vsync_sample) return Error(kUnsupported);
    sample->timestampNs = context->state.last_vsync_ns;
    sample->vsyncPeriodNs = context->device->period_ns();
    return ndk::ScopedAStatus::ok();
}

int64_t ComposerClient::MonotonicNanos() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

void ComposerClient::VsyncLoop(int64_t display) {
    DisplayContext* context = GetDisplay(display);
    int64_t next = MonotonicNanos();
    while (!stopping_) {
        std::shared_ptr<IComposerCallback> callback;
        const int32_t period = context->device->period_ns();
        {
            std::unique_lock lock(mutex_);
            vsync_cv_.wait(lock,
                           [this, context] { return stopping_ || context->state.vsync_enabled; });
            if (stopping_) break;
            callback = callback_;
        }
        int64_t timestamp = 0;
        const fb::FbdevDevice::VsyncWaitResult wait = context->device->WaitForVsync(&timestamp);
        if (wait == fb::FbdevDevice::VsyncWaitResult::kInterrupted) continue;
        if (wait == fb::FbdevDevice::VsyncWaitResult::kFallback) {
            std::unique_lock lock(mutex_);
            next = std::max(next + period, MonotonicNanos());
            const auto deadline =
                    std::chrono::steady_clock::time_point(std::chrono::nanoseconds(next));
            if (vsync_cv_.wait_until(lock, deadline, [this, context] {
                    return stopping_ || !context->state.vsync_enabled;
                })) {
                continue;
            }
            timestamp = MonotonicNanos();
            callback = callback_;
        } else {
            next = timestamp;
            std::lock_guard lock(mutex_);
            context->state.have_vsync_sample = true;
            context->state.last_vsync_ns = timestamp;
        }
        if (callback != nullptr) {
            std::lock_guard callback_lock(vsync_callback_mutex_);
            {
                std::lock_guard lock(mutex_);
                if (!context->state.vsync_enabled) continue;
            }
            const auto status = callback->onVsync(display, timestamp, period);
            if (!status.isOk()) {
                ALOGW("Vsync callback failed for display %" PRId64 ": %s", display,
                      status.getDescription().c_str());
            }
        }
    }
    ALOGI("Vsync worker stopped");
}

std::string ComposerClient::Dump() {
    std::lock_guard lock(mutex_);
    std::ostringstream out;
    out << "fbdev Composer3 V5 displays=" << displays_.size() << '\n';
    for (size_t id = 0; id < displays_.size(); ++id) {
        const DisplayContext& display = *displays_[id];
        out << display.device->Dump() << " display=" << id
            << " config=0 layers=" << display.state.layers.size()
            << " targetSlots=" << display.state.target_slots.size()
            << " target=" << (display.state.target ? display.state.target->view().allocation_id : 0)
            << " validation=" << static_cast<int>(display.state.validation)
            << " vsync=" << display.state.vsync_enabled
            << " refreshDebug=" << display.state.refresh_debug_enabled << '\n';
    }
    return out.str();
}

ndk::SpAIBinder ComposerClient::createBinder() {
    auto binder = BnComposerClient::createBinder();
    AIBinder_setInheritRt(binder.get(), true);
    return binder;
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
