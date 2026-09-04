/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Module"

#include "ModuleMainline.h"

#include <Log.h>
#include <android-base/file.h>
#include <core-impl/utils.h>

#include "alsa/AlsaMixer.h"
#include "routing/ConfigurationBuilder.h"

namespace aidl::android::hardware::audio::core::mainline {

using ::aidl::android::hardware::audio::common::SinkMetadata;
using ::aidl::android::hardware::audio::common::SourceMetadata;
using ::aidl::android::media::audio::common::AudioDevice;
using ::aidl::android::media::audio::common::AudioDeviceDescription;
using ::aidl::android::media::audio::common::AudioOffloadInfo;
using ::aidl::android::media::audio::common::AudioPort;
using ::aidl::android::media::audio::common::AudioPortConfig;
using ::aidl::android::media::audio::common::AudioPortExt;
using ::aidl::android::media::audio::common::MicrophoneInfo;

namespace {

bool IsUsbDevicePort(const AudioPort& port) {
    return port.ext.getTag() == AudioPortExt::Tag::device &&
           port.ext.get<AudioPortExt::Tag::device>().device.type.connection ==
                   AudioDeviceDescription::CONNECTION_USB;
}

}  // namespace

std::shared_ptr<ModuleMainline> ModuleMainline::Create(const Properties& properties) {
    std::shared_ptr<routing::DeviceInventory> inventory =
            routing::DeviceInventory::Discover(properties);
    std::unique_ptr<Configuration> config = routing::BuildConfiguration(*inventory, properties);
    return ndk::SharedRefBase::make<ModuleMainline>(std::move(config), properties,
                                                    std::move(inventory));
}

ModuleMainline::ModuleMainline(std::unique_ptr<Configuration>&& config,
                               const Properties& properties,
                               std::shared_ptr<routing::DeviceInventory> inventory)
    : Module(Type::DEFAULT, std::move(config)),
      properties_(properties),
      inventory_(std::move(inventory)),
      routing_(std::make_shared<routing::RoutingController>(inventory_)),
      mic_muted_(std::make_shared<std::atomic<bool>>(false)) {
    LOG(INFO) << __func__ << ": module ready";
}

StreamDeps ModuleMainline::MakeStreamDeps() const {
    return StreamDeps{.inventory = inventory_, .routing = routing_, .mic_muted = mic_muted_};
}

// --- Optional sub-interfaces -------------------------------------------------

ndk::ScopedAStatus ModuleMainline::getTelephony(std::shared_ptr<ITelephony>* _aidl_return) {
    // Voice calls need modem specific plumbing that a generic HAL can not offer.
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModuleMainline::getBluetooth(std::shared_ptr<IBluetooth>* _aidl_return) {
    // Bluetooth is served by the dedicated "bluetooth" module instance.
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModuleMainline::getBluetoothA2dp(std::shared_ptr<IBluetoothA2dp>* _aidl_return) {
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModuleMainline::getBluetoothLe(std::shared_ptr<IBluetoothLe>* _aidl_return) {
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::ok();
}

// --- Volume / mute -----------------------------------------------------------

// Master volume and mute are global to the module while the cards behind it
// are many and heterogeneous; the framework applies both in the digital domain
// when the HAL reports them as unsupported, which is exact and consistent.

ndk::ScopedAStatus ModuleMainline::getMasterMute(bool* /*_aidl_return*/) {
    LOG(DEBUG) << __func__ << ": not supported, handled by the framework";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ModuleMainline::setMasterMute(bool /*in_mute*/) {
    LOG(DEBUG) << __func__ << ": not supported, handled by the framework";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ModuleMainline::getMasterVolume(float* /*_aidl_return*/) {
    LOG(DEBUG) << __func__ << ": not supported, handled by the framework";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ModuleMainline::setMasterVolume(float /*in_volume*/) {
    LOG(DEBUG) << __func__ << ": not supported, handled by the framework";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ModuleMainline::getMicMute(bool* _aidl_return) {
    *_aidl_return = mic_muted_->load(std::memory_order_relaxed);
    LOG(DEBUG) << __func__ << ": " << *_aidl_return;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModuleMainline::setMicMute(bool in_mute) {
    // Applied by the input streams: captured data is replaced by silence.
    LOG(INFO) << __func__ << ": " << in_mute;
    mic_muted_->store(in_mute, std::memory_order_relaxed);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ModuleMainline::getSupportedPlaybackRateFactors(
        SupportedPlaybackRateFactors* /*_aidl_return*/) {
    LOG(DEBUG) << __func__ << ": not supported";
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus ModuleMainline::setAudioPortConfig(const AudioPortConfig& in_requested,
                                                      AudioPortConfig* out_suggested,
                                                      bool* _aidl_return) {
    // Module validates a non-null `gain` against the port's gain controllers
    // and rejects the whole request when the port has none. Our ports have no
    // gain controllers (volume is applied by the framework), and the framework
    // sometimes carries a placeholder gain without any value over from an
    // earlier config. Such a placeholder asks for nothing, so drop it instead
    // of failing the stream open that depends on this config.
    if (in_requested.gain.has_value() && in_requested.gain->values.empty()) {
        auto& ports = getConfig().ports;
        const int32_t port_id = in_requested.portId != 0 ? in_requested.portId : [&] {
            auto& configs = getConfig().portConfigs;
            const auto it = findById<AudioPortConfig>(configs, in_requested.id);
            return it != configs.end() ? it->portId : 0;
        }();
        const auto port = findById<AudioPort>(ports, port_id);
        if (port != ports.end() && port->gains.empty()) {
            AudioPortConfig sanitized = in_requested;
            sanitized.gain = std::nullopt;
            return Module::setAudioPortConfig(sanitized, out_suggested, _aidl_return);
        }
    }
    return Module::setAudioPortConfig(in_requested, out_suggested, _aidl_return);
}

// --- Streams -----------------------------------------------------------------

ndk::ScopedAStatus ModuleMainline::createInputStream(StreamContext&& context,
                                                     const SinkMetadata& sink_metadata,
                                                     const std::vector<MicrophoneInfo>& microphones,
                                                     std::shared_ptr<StreamIn>* result) {
    return createStreamInstance<StreamInMainline>(result, std::move(context), sink_metadata,
                                                  microphones, MakeStreamDeps());
}

ndk::ScopedAStatus ModuleMainline::createOutputStream(
        StreamContext&& context, const SourceMetadata& source_metadata,
        const std::optional<AudioOffloadInfo>& offload_info, std::shared_ptr<StreamOut>* result) {
    if (offload_info.has_value()) {
        LOG(ERROR) << __func__ << ": compressed offload is not supported";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return createStreamInstance<StreamOutMainline>(result, std::move(context), source_metadata,
                                                   offload_info, MakeStreamDeps());
}

int32_t ModuleMainline::getNominalLatencyMs(const AudioPortConfig& /*port_config*/) {
    return properties_.latency_ms;
}

// --- External devices --------------------------------------------------------

ndk::ScopedAStatus ModuleMainline::populateConnectedDevicePort(AudioPort* audio_port,
                                                               int32_t /*next_port_id*/) {
    if (audio_port->ext.getTag() != AudioPortExt::Tag::device) {
        LOG(ERROR) << __func__ << ": port " << audio_port->id << " is not a device port";
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const AudioDevice& device = audio_port->ext.get<AudioPortExt::Tag::device>().device;
    const bool is_input = audio_port->flags.getTag() ==
                          ::aidl::android::media::audio::common::AudioIoFlags::input;

    if (IsUsbDevicePort(*audio_port)) {
        // The framework tells us which ALSA card / device the USB accessory
        // became; probe it for what it can do.
        auto endpoint = inventory_->MakeUsbEndpoint(device, is_input);
        if (!endpoint.has_value()) {
            LOG(ERROR) << __func__ << ": USB device " << device.toString() << " can not be used";
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        audio_port->profiles = endpoint->profiles;
        LOG(INFO) << __func__ << ": USB port " << audio_port->id << " -> " << endpoint->ToString();
        return ndk::ScopedAStatus::ok();
    }

    // Wired headphones, HDMI, ...: the template is backed by a fixed ALSA path
    // whose capabilities were probed at start-up. At this point `audio_port`
    // still carries the id of the template.
    const routing::Endpoint* endpoint = inventory_->FindByPortId(audio_port->id);
    if (endpoint == nullptr) {
        LOG(ERROR) << __func__ << ": no endpoint behind template port " << audio_port->id;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    audio_port->profiles = endpoint->profiles;
    LOG(INFO) << __func__ << ": port " << audio_port->id << " connected -> "
              << endpoint->ToString();
    return ndk::ScopedAStatus::ok();
}

void ModuleMainline::onExternalDeviceConnectionChanged(const AudioPort& audio_port,
                                                       bool connected) {
    if (!connected || !IsUsbDevicePort(audio_port)) return;
    // A freshly plugged USB card comes up with whatever mixer state the
    // firmware has, often muted. Bring it into a usable state, like the
    // example HAL's UsbAlsaMixerControl does.
    if (!properties_.mixer_init) return;
    const bool is_input =
            audio_port.flags.getTag() == ::aidl::android::media::audio::common::AudioIoFlags::input;
    auto endpoint = inventory_->MakeUsbEndpoint(
            audio_port.ext.get<AudioPortExt::Tag::device>().device, is_input);
    if (!endpoint.has_value()) return;
    alsa::MixerInitOptions options;
    options.playback_percent = properties_.mixer_playback_percent;
    options.capture_percent = properties_.mixer_capture_percent;
    alsa::InitializeMixer(endpoint->card, options);
}

// --- Debugging ---------------------------------------------------------------

binder_status_t ModuleMainline::dump(int fd, const char** args, uint32_t num_args) {
    std::string text = "Mainline audio HAL\n";
    text += "Properties: " + properties_.ToString() + "\n";
    text += inventory_->Dump();
    text += routing_->Dump();
    ::android::base::WriteStringToFd(text, fd);
    return Module::dump(fd, args, num_args);
}

}  // namespace aidl::android::hardware::audio::core::mainline
