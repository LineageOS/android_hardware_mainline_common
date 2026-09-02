/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/graphics/composer3/BnComposerClient.h>
#include <android-base/unique_fd.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "FbdevDevice.h"

namespace aidl::android::hardware::graphics::composer3::impl {

class ComposerClient : public BnComposerClient {
  public:
    explicit ComposerClient(std::string path);
    ~ComposerClient() override;
    bool Init();
    std::string Dump();

    ndk::ScopedAStatus createLayer(int64_t display, int32_t slots, int64_t* layer) override;
    ndk::ScopedAStatus createVirtualDisplay(int32_t width, int32_t height,
                                            common::PixelFormat format, int32_t slots,
                                            VirtualDisplay* display) override;
    ndk::ScopedAStatus destroyLayer(int64_t display, int64_t layer) override;
    ndk::ScopedAStatus destroyVirtualDisplay(int64_t display) override;
    ndk::ScopedAStatus executeCommands(const std::vector<DisplayCommand>& commands,
                                       std::vector<CommandResultPayload>* results) override;
    ndk::ScopedAStatus getActiveConfig(int64_t display, int32_t* config) override;
    ndk::ScopedAStatus getColorModes(int64_t display, std::vector<ColorMode>* modes) override;
    ndk::ScopedAStatus getDataspaceSaturationMatrix(common::Dataspace dataspace,
                                                    std::vector<float>* matrix) override;
    ndk::ScopedAStatus getDisplayAttribute(int64_t display, int32_t config,
                                           DisplayAttribute attribute, int32_t* value) override;
    ndk::ScopedAStatus getDisplayCapabilities(int64_t display,
                                              std::vector<DisplayCapability>* caps) override;
    ndk::ScopedAStatus getDisplayConfigs(int64_t display, std::vector<int32_t>* configs) override;
    ndk::ScopedAStatus getDisplayConnectionType(int64_t display,
                                                DisplayConnectionType* type) override;
    ndk::ScopedAStatus getDisplayIdentificationData(int64_t display,
                                                    DisplayIdentification* data) override;
    ndk::ScopedAStatus getDisplayName(int64_t display, std::string* name) override;
    ndk::ScopedAStatus getDisplayVsyncPeriod(int64_t display, int32_t* period) override;
    ndk::ScopedAStatus getDisplayedContentSample(int64_t display, int64_t max_frames,
                                                 int64_t timestamp,
                                                 DisplayContentSample* sample) override;
    ndk::ScopedAStatus getDisplayedContentSamplingAttributes(
            int64_t display, DisplayContentSamplingAttributes* attributes) override;
    ndk::ScopedAStatus getDisplayPhysicalOrientation(int64_t display,
                                                     common::Transform* orientation) override;
    ndk::ScopedAStatus getHdrCapabilities(int64_t display, HdrCapabilities* caps) override;
    ndk::ScopedAStatus getMaxVirtualDisplayCount(int32_t* count) override;
    ndk::ScopedAStatus getPerFrameMetadataKeys(int64_t display,
                                               std::vector<PerFrameMetadataKey>* keys) override;
    ndk::ScopedAStatus getReadbackBufferAttributes(int64_t display,
                                                   ReadbackBufferAttributes* attributes) override;
    ndk::ScopedAStatus getReadbackBufferFence(int64_t display,
                                              ndk::ScopedFileDescriptor* fence) override;
    ndk::ScopedAStatus getRenderIntents(int64_t display, ColorMode mode,
                                        std::vector<RenderIntent>* intents) override;
    ndk::ScopedAStatus getSupportedContentTypes(int64_t display,
                                                std::vector<ContentType>* types) override;
    ndk::ScopedAStatus getDisplayDecorationSupport(
            int64_t display, std::optional<common::DisplayDecorationSupport>* support) override;
    ndk::ScopedAStatus registerCallback(
            const std::shared_ptr<IComposerCallback>& callback) override;
    ndk::ScopedAStatus setActiveConfig(int64_t display, int32_t config) override;
    ndk::ScopedAStatus setActiveConfigWithConstraints(
            int64_t display, int32_t config, const VsyncPeriodChangeConstraints& constraints,
            VsyncPeriodChangeTimeline* timeline) override;
    ndk::ScopedAStatus setBootDisplayConfig(int64_t display, int32_t config) override;
    ndk::ScopedAStatus clearBootDisplayConfig(int64_t display) override;
    ndk::ScopedAStatus getPreferredBootDisplayConfig(int64_t display, int32_t* config) override;
    ndk::ScopedAStatus setAutoLowLatencyMode(int64_t display, bool on) override;
    ndk::ScopedAStatus setClientTargetSlotCount(int64_t display, int32_t count) override;
    ndk::ScopedAStatus setColorMode(int64_t display, ColorMode mode, RenderIntent intent) override;
    ndk::ScopedAStatus setContentType(int64_t display, ContentType type) override;
    ndk::ScopedAStatus setDisplayedContentSamplingEnabled(int64_t display, bool enable,
                                                          FormatColorComponent component,
                                                          int64_t max_frames) override;
    ndk::ScopedAStatus setPowerMode(int64_t display, PowerMode mode) override;
    ndk::ScopedAStatus setReadbackBuffer(
            int64_t display, const ::aidl::android::hardware::common::NativeHandle& buffer,
            const ndk::ScopedFileDescriptor& fence) override;
    ndk::ScopedAStatus setVsyncEnabled(int64_t display, bool enabled) override;
    ndk::ScopedAStatus setIdleTimerEnabled(int64_t display, int32_t timeout_ms) override;
    ndk::ScopedAStatus getOverlaySupport(OverlayProperties* properties) override;
    ndk::ScopedAStatus getHdrConversionCapabilities(
            std::vector<common::HdrConversionCapability>* capabilities) override;
    ndk::ScopedAStatus setHdrConversionStrategy(const common::HdrConversionStrategy& strategy,
                                                common::Hdr* hdr) override;
    ndk::ScopedAStatus setRefreshRateChangedCallbackDebugEnabled(int64_t display,
                                                                 bool enabled) override;
    ndk::ScopedAStatus getDisplayConfigurations(
            int64_t display, int32_t max_frame_interval_ns,
            std::vector<DisplayConfiguration>* configurations) override;
    ndk::ScopedAStatus notifyExpectedPresent(int64_t display,
                                             const ClockMonotonicTimestamp& expected,
                                             int32_t frame_interval_ns) override;
    ndk::ScopedAStatus startHdcpNegotiation(int64_t display,
                                            const drm::HdcpLevels& levels) override;
    ndk::ScopedAStatus getMaxLayerPictureProfiles(int64_t display, int32_t* max_profiles) override;
    ndk::ScopedAStatus getLuts(int64_t display, const std::vector<Buffer>& buffers,
                               std::vector<Luts>* luts) override;
    ndk::ScopedAStatus getDisplayKnownVsyncSample(int64_t display, VsyncSample* sample) override;

  protected:
    ndk::SpAIBinder createBinder() override;

  private:
    enum class ValidationState { kDirty, kAwaitingAccept, kValidated };
    struct LayerState {
        Composition composition = Composition::INVALID;
        int32_t slots = 0;
    };
    struct DisplayState {
        int64_t next_layer = 1;
        std::map<int64_t, LayerState> layers;
        std::vector<std::shared_ptr<fb::ImportedBuffer>> target_slots;
        std::shared_ptr<fb::ImportedBuffer> target;
        std::vector<fb::DamageRect> target_damage;
        ::android::base::unique_fd target_fence;
        ValidationState validation = ValidationState::kDirty;
        bool vsync_enabled = false;
        bool refresh_debug_enabled = false;
        bool have_vsync_sample = false;
        int64_t last_vsync_ns = 0;
    };
    struct DisplayContext {
        std::unique_ptr<fb::FbdevDevice> device;
        DisplayState state;
        std::thread vsync_thread;
    };

    static ndk::ScopedAStatus Error(int32_t error);
    static int64_t MonotonicNanos();
    bool IsDisplay(int64_t display) const;
    DisplayContext* GetDisplay(int64_t display);
    const DisplayContext* GetDisplay(int64_t display) const;
    ndk::ScopedAStatus UnsupportedDisplay(int64_t display);
    bool ApplyLayerLocked(DisplayState* state, const LayerCommand& command, int32_t* error);
    bool SetClientTargetLocked(DisplayContext* display, const ClientTarget& target, int32_t* error);
    void AddError(size_t index, int32_t error, std::vector<CommandResultPayload>* results);
    void ValidateLocked(int64_t display, DisplayState* state,
                        std::vector<CommandResultPayload>* results);
    bool PresentLocked(int64_t display, DisplayContext* context,
                       std::vector<CommandResultPayload>* results, int32_t* error);
    void VsyncLoop(int64_t display);

    const std::string path_;
    mutable std::mutex mutex_;
    std::mutex callback_mutex_;
    std::mutex refresh_callback_mutex_;
    std::mutex vsync_callback_mutex_;
    std::condition_variable vsync_cv_;
    std::vector<std::unique_ptr<DisplayContext>> displays_;
    std::shared_ptr<IComposerCallback> callback_;
    std::atomic<bool> stopping_{false};
};

}  // namespace aidl::android::hardware::graphics::composer3::impl
