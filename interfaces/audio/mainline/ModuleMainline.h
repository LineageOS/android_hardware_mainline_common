/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include <core-impl/Module.h>

#include "Properties.h"
#include "passthrough/HdmiControl.h"
#include "passthrough/PassthroughSink.h"
#include "passthrough/SinkCapabilities.h"
#include "routing/DeviceInventory.h"
#include "routing/PcmArbiter.h"
#include "routing/RoutingController.h"
#include "stream/StreamMainline.h"

namespace aidl::android::hardware::audio::core::mainline {

// The "default" (primary) module: every sound card selected at start-up plus
// the USB device port templates. Builds on the example HAL's Module for the
// port / patch / stream bookkeeping and adds the alsa-lib / UCM backend.
class ModuleMainline final : public Module {
  public:
    // Discovers the hardware and creates the module. Never fails: without any
    // sound card the module exposes null devices.
    static std::shared_ptr<ModuleMainline> Create(const Properties& properties);

    ModuleMainline(std::unique_ptr<Configuration>&& config, const Properties& properties,
                   std::shared_ptr<routing::DeviceInventory> inventory);

  private:
    // IModule
    ndk::ScopedAStatus getTelephony(std::shared_ptr<ITelephony>* _aidl_return) override;
    ndk::ScopedAStatus getBluetooth(std::shared_ptr<IBluetooth>* _aidl_return) override;
    ndk::ScopedAStatus getBluetoothA2dp(std::shared_ptr<IBluetoothA2dp>* _aidl_return) override;
    ndk::ScopedAStatus getBluetoothLe(std::shared_ptr<IBluetoothLe>* _aidl_return) override;
    ndk::ScopedAStatus getMasterMute(bool* _aidl_return) override;
    ndk::ScopedAStatus setMasterMute(bool in_mute) override;
    ndk::ScopedAStatus getMasterVolume(float* _aidl_return) override;
    ndk::ScopedAStatus setMasterVolume(float in_volume) override;
    ndk::ScopedAStatus getMicMute(bool* _aidl_return) override;
    ndk::ScopedAStatus setMicMute(bool in_mute) override;
    ndk::ScopedAStatus getSupportedPlaybackRateFactors(
            SupportedPlaybackRateFactors* _aidl_return) override;
    ndk::ScopedAStatus setAudioPortConfig(
            const ::aidl::android::media::audio::common::AudioPortConfig& in_requested,
            ::aidl::android::media::audio::common::AudioPortConfig* out_suggested,
            bool* _aidl_return) override;
    binder_status_t dump(int fd, const char** args, uint32_t num_args) override;

    // Module extension points
    ndk::ScopedAStatus createInputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sink_metadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones,
            std::shared_ptr<StreamIn>* result) override;
    ndk::ScopedAStatus createOutputStream(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& source_metadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                    offload_info,
            std::shared_ptr<StreamOut>* result) override;
    ndk::ScopedAStatus populateConnectedDevicePort(
            ::aidl::android::media::audio::common::AudioPort* audio_port,
            int32_t next_port_id) override;
    void onExternalDeviceConnectionChanged(
            const ::aidl::android::media::audio::common::AudioPort& audio_port,
            bool connected) override;
    int32_t getNominalLatencyMs(
            const ::aidl::android::media::audio::common::AudioPortConfig& port_config) override;
    ndk::ScopedAStatus calculateBufferSizeFrames(
            const ::aidl::android::media::audio::common::AudioFormatDescription& format,
            const ::aidl::android::media::audio::common::AudioIoFlags& flags, int32_t latency_ms,
            int32_t sample_rate, int32_t* buffer_size_frames) override;

    StreamDeps MakeStreamDeps() const;
    // Id of the mix port called `name`, 0 when there is none.
    int32_t FindMixPort(const char* name);
    // Id of the primary output mix port when it carries the FAST flag, else 0.
    int32_t FindFastOutputPort();
    // Adds the passthrough profiles and encoded formats of the sink behind
    // `head` to the connected HDMI device port.
    void AddPassthroughProfiles(const routing::Endpoint& head,
                                ::aidl::android::media::audio::common::AudioPort* audio_port);
    passthrough::SinkCapabilities ResolveSinkCapabilities(passthrough::HdmiControl& control);
    // Fills the passthrough mix port from the connected HDMI device port, or
    // empties it again (dynamic) on disconnection.
    void UpdatePassthroughMixPort(const ::aidl::android::media::audio::common::AudioPort& hdmi_port,
                                  bool connected);
    // Completes the policy's probe of the dynamic passthrough mix port, see
    // setAudioPortConfig().
    void CompletePassthroughProbe(::aidl::android::media::audio::common::AudioPortConfig* request);

    const Properties properties_;
    const std::shared_ptr<routing::DeviceInventory> inventory_;
    const std::shared_ptr<routing::RoutingController> routing_;
    const std::shared_ptr<routing::PcmArbiter> pcm_arbiter_;
    const std::shared_ptr<std::atomic<bool>> mic_muted_;
    const int32_t fast_output_port_id_;
    // HDMI heads that failed high bit rate, shared with the streams.
    const std::shared_ptr<passthrough::HbrFailures> hbr_failures_;
    const passthrough::HdmiControlFactory hdmi_control_factory_;
    // From hdmi.passthrough_formats; empty: use the ELD.
    const std::vector<passthrough::EncodedFormat> forced_passthrough_formats_;
    // 0 without the "hdmi passthrough" mix port.
    const int32_t passthrough_port_id_;
};

}  // namespace aidl::android::hardware::audio::core::mainline
