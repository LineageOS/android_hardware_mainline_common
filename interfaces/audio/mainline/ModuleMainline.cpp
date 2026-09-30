/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Module"

#include "ModuleMainline.h"

#include <algorithm>
#include <map>

#include <Log.h>
#include <Utils.h>
#include <android-base/file.h>
#include <core-impl/utils.h>

#include "alsa/AlsaMixer.h"
#include "passthrough/Eld.h"
#include "passthrough/Format.h"
#include "routing/ConfigurationBuilder.h"

namespace aidl::android::hardware::audio::core::mainline {

using ::aidl::android::hardware::audio::common::isBitPositionFlagSet;
using ::aidl::android::hardware::audio::common::SinkMetadata;
using ::aidl::android::hardware::audio::common::SourceMetadata;
using ::aidl::android::media::audio::common::AudioDevice;
using ::aidl::android::media::audio::common::AudioDeviceDescription;
using ::aidl::android::media::audio::common::AudioFormatDescription;
using ::aidl::android::media::audio::common::AudioIoFlags;
using ::aidl::android::media::audio::common::AudioOffloadInfo;
using ::aidl::android::media::audio::common::AudioOutputFlags;
using ::aidl::android::media::audio::common::AudioPort;
using ::aidl::android::media::audio::common::AudioPortConfig;
using ::aidl::android::media::audio::common::AudioPortExt;
using ::aidl::android::media::audio::common::AudioProfile;
using ::aidl::android::media::audio::common::Int;
using ::aidl::android::media::audio::common::MicrophoneInfo;

namespace {

bool IsUsbDevicePort(const AudioPort& port) {
    return port.ext.getTag() == AudioPortExt::Tag::device &&
           port.ext.get<AudioPortExt::Tag::device>().device.type.connection ==
                   AudioDeviceDescription::CONNECTION_USB;
}

bool IsHdmiDevicePort(const AudioPort& port) {
    return port.ext.getTag() == AudioPortExt::Tag::device &&
           port.flags.getTag() == AudioIoFlags::Tag::output &&
           port.ext.get<AudioPortExt::Tag::device>().device.type.connection ==
                   AudioDeviceDescription::CONNECTION_HDMI;
}

// Creates the HdmiControl of a head on one of the inventory's cards. Safe to
// call from any thread: the inventory does not change and HbrFailures is
// thread safe.
passthrough::HdmiControlFactory MakeHdmiControlFactory(
        std::shared_ptr<routing::DeviceInventory> inventory,
        std::shared_ptr<passthrough::HbrFailures> hbr_failures) {
    std::map<int, std::optional<bool>> hbr_overrides;
    for (const alsa::CardInfo& card : inventory->cards()) {
        hbr_overrides[card.index] =
                Properties::LoadCardProperties(card.id, card.index, card.name).hbr;
    }
    return [inventory = std::move(inventory), hbr_failures = std::move(hbr_failures),
            hbr_overrides = std::move(hbr_overrides)](
                   const routing::Endpoint& head) -> std::unique_ptr<passthrough::HdmiControl> {
        const auto& cards = inventory->cards();
        const auto card = std::find_if(cards.begin(), cards.end(),
                                       [&head](const auto& c) { return c.index == head.card; });
        if (card == cards.end()) return nullptr;
        const auto hbr = hbr_overrides.find(head.card);
        return passthrough::CreateHdmiControl(
                *card, head, hbr_failures, hbr != hbr_overrides.end() ? hbr->second : std::nullopt);
    };
}

std::vector<passthrough::EncodedFormat> ParseFormatList(const std::vector<std::string>& names) {
    std::vector<passthrough::EncodedFormat> formats;
    for (const std::string& name : names) {
        if (const auto format = passthrough::EncodedFormatFromString(name); format.has_value()) {
            formats.push_back(*format);
        } else {
            LOG(WARNING) << "hdmi.passthrough_formats: unknown format \"" << name << "\"";
        }
    }
    return formats;
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
      pcm_arbiter_(std::make_shared<routing::PcmArbiter>()),
      mic_muted_(std::make_shared<std::atomic<bool>>(false)),
      fast_output_port_id_(FindFastOutputPort()),
      hbr_failures_(std::make_shared<passthrough::HbrFailures>()),
      hdmi_control_factory_(MakeHdmiControlFactory(inventory_, hbr_failures_)),
      forced_passthrough_formats_(ParseFormatList(properties.hdmi_passthrough_formats)),
      passthrough_port_id_(FindMixPort(routing::kPassthroughOutputMixPort)) {
    LOG(INFO) << __func__ << ": module ready";
}

int32_t ModuleMainline::FindMixPort(const char* name) {
    for (const AudioPort& port : getConfig().ports) {
        if (port.name == name && port.ext.getTag() == AudioPortExt::Tag::mix) return port.id;
    }
    return 0;
}

int32_t ModuleMainline::FindFastOutputPort() {
    const int32_t id = FindMixPort(routing::kPrimaryOutputMixPort);
    auto& ports = getConfig().ports;
    const auto port = findById<AudioPort>(ports, id);
    if (port == ports.end() || port->flags.getTag() != AudioIoFlags::Tag::output) return 0;
    return isBitPositionFlagSet(port->flags.get<AudioIoFlags::Tag::output>(),
                                AudioOutputFlags::FAST)
                   ? id
                   : 0;
}

StreamDeps ModuleMainline::MakeStreamDeps() const {
    return StreamDeps{.inventory = inventory_,
                      .routing = routing_,
                      .pcm_arbiter = pcm_arbiter_,
                      .mic_muted = mic_muted_,
                      .make_hdmi_control = hdmi_control_factory_};
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
    AudioPortConfig request = in_requested;
    // Module validates a non-null `gain` against the port's gain controllers
    // and rejects the whole request when the port has none. Our ports have no
    // gain controllers (volume is applied by the framework), and the framework
    // sometimes carries a placeholder gain without any value over from an
    // earlier config. Such a placeholder asks for nothing, so drop it instead
    // of failing the stream open that depends on this config.
    if (request.gain.has_value() && request.gain->values.empty()) {
        auto& ports = getConfig().ports;
        const int32_t port_id = request.portId != 0 ? request.portId : [&] {
            auto& configs = getConfig().portConfigs;
            const auto it = findById<AudioPortConfig>(configs, request.id);
            return it != configs.end() ? it->portId : 0;
        }();
        const auto port = findById<AudioPort>(ports, port_id);
        if (port != ports.end() && port->gains.empty()) request.gain = std::nullopt;
    }
    if (request.id == 0 && passthrough_port_id_ != 0 && request.portId == passthrough_port_id_) {
        CompletePassthroughProbe(&request);
    }
    return Module::setAudioPortConfig(request, out_suggested, _aidl_return);
}

// The policy learns the formats of a dynamic mix port by opening it once with
// an empty configuration, then asking for the port (updateAudioProfiles()).
// Module answers such a request with a suggestion that is not applied, and
// for a DIRECT port with a non-PCM (here: unset) format neither libaudiohal
// (Hal2AidlMapper) nor the policy retries, so the probe fails and the formats
// never reach the policy. Apply the first profile instead; the policy closes
// the probe and reopens with a configuration of its choice.
void ModuleMainline::CompletePassthroughProbe(AudioPortConfig* request) {
    if (request->format.has_value() || request->channelMask.has_value() ||
        request->sampleRate.has_value()) {
        return;
    }
    auto& ports = getConfig().ports;
    const auto port = findById<AudioPort>(ports, passthrough_port_id_);
    if (port == ports.end() || port->profiles.empty()) return;
    const AudioProfile& profile = port->profiles.front();
    if (profile.channelMasks.empty() || profile.sampleRates.empty()) return;
    request->format = profile.format;
    request->channelMask = profile.channelMasks.front();
    request->sampleRate = Int{.value = profile.sampleRates.front()};
    LOG(INFO) << __func__ << ": answering the probe of \"" << port->name << "\" with "
              << profile.format.toString();
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

ndk::ScopedAStatus ModuleMainline::calculateBufferSizeFrames(const AudioFormatDescription& format,
                                                             const AudioIoFlags& flags,
                                                             int32_t latency_ms,
                                                             int32_t sample_rate,
                                                             int32_t* buffer_size_frames) {
    // Module only sizes PCM buffers.
    if (const auto frames = passthrough::BufferSizeFrames(format, latency_ms, sample_rate);
        frames.has_value()) {
        *buffer_size_frames = *frames;
        return ndk::ScopedAStatus::ok();
    }
    return Module::calculateBufferSizeFrames(format, flags, latency_ms, sample_rate,
                                             buffer_size_frames);
}

int32_t ModuleMainline::getNominalLatencyMs(const AudioPortConfig& port_config) {
    // The buffer size, and with it whether AudioFlinger runs a FastMixer on
    // the output, follows from this latency.
    if (fast_output_port_id_ != 0 && port_config.portId == fast_output_port_id_) {
        return properties_.fast_latency_ms;
    }
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
    const bool is_hdmi_template = endpoint->role == routing::DeviceRole::kHdmi;
    endpoint = inventory_->SelectHdmiEndpoint(*endpoint);
    audio_port->profiles = endpoint->profiles;
    if (is_hdmi_template && passthrough_port_id_ != 0) {
        AddPassthroughProfiles(*endpoint, audio_port);
    }
    LOG(INFO) << __func__ << ": port " << audio_port->id << " connected -> "
              << endpoint->ToString();
    return ndk::ScopedAStatus::ok();
}

passthrough::SinkCapabilities ModuleMainline::ResolveSinkCapabilities(
        passthrough::HdmiControl& control) {
    if (!forced_passthrough_formats_.empty()) {
        return passthrough::SinkCapabilities::FromFormatList(forced_passthrough_formats_);
    }
    if (const auto eld = control.ReadEld(); eld.has_value()) {
        if (auto caps = passthrough::ParseEld(*eld); caps.has_value()) return *caps;
    }
    return passthrough::SinkCapabilities{};
}

void ModuleMainline::AddPassthroughProfiles(const routing::Endpoint& head, AudioPort* audio_port) {
    // Without a control there is nothing to offer, but IEC 61937 still goes
    // in: the passthrough mix port must not stay without profiles (see
    // UpdatePassthroughMixPort()), and opening it then fails cleanly.
    passthrough::SinkCapabilities sink;
    bool hbr = false;
    std::string control_description = "no passthrough controls";
    if (auto control = hdmi_control_factory_(head); control != nullptr) {
        sink = ResolveSinkCapabilities(*control);
        hbr = control->Hbr() == passthrough::HbrSupport::kSupported;
        control_description = control->Describe();
    }
    const std::vector<AudioProfile> profiles = passthrough::PassthroughProfiles(sink, hbr);
    audio_port->profiles.insert(audio_port->profiles.end(), profiles.begin(), profiles.end());
    audio_port->ext.get<AudioPortExt::Tag::device>().encodedFormats =
            passthrough::EncodedFormatsOf(profiles);
    LOG(INFO) << __func__ << ": " << control_description << ", sink " << sink.ToString() << ", "
              << profiles.size() << " passthrough profile(s)";
}

void ModuleMainline::UpdatePassthroughMixPort(const AudioPort& hdmi_port, bool connected) {
    auto& ports = getConfig().ports;
    const auto port = findById<AudioPort>(ports, passthrough_port_id_);
    if (port == ports.end()) return;
    port->profiles.clear();
    if (connected) {
        // Encoded formats only: listing PCM here would let AudioFlinger's
        // own IEC 61937 wrapper (SpdifStreamOut) reopen the port as PCM,
        // which the HAL could not tell from real PCM.
        for (const AudioProfile& profile : hdmi_port.profiles) {
            if (passthrough::FromAidl(profile.format).has_value())
                port->profiles.push_back(profile);
        }
        // Also when populateConnectedDevicePort() did not run (connection
        // simulation): a routable mix port left without profiles would get
        // all of the device's, PCM included, from Module.
        if (port->profiles.empty()) {
            port->profiles = passthrough::PassthroughProfiles(passthrough::SinkCapabilities{},
                                                              false /*hbr*/);
        }
    }
    LOG(INFO) << __func__ << ": \"" << port->name << "\" has " << port->profiles.size()
              << " profile(s)";
}

void ModuleMainline::onExternalDeviceConnectionChanged(const AudioPort& audio_port,
                                                       bool connected) {
    // Runs before Module copies the connected device's profiles into the
    // routable mix ports that have none, so a filled passthrough port is left
    // alone, and Module does not clear it on disconnection either.
    if (passthrough_port_id_ != 0 && IsHdmiDevicePort(audio_port)) {
        UpdatePassthroughMixPort(audio_port, connected);
    }
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
    text += pcm_arbiter_->Dump();
    ::android::base::WriteStringToFd(text, fd);
    return Module::dump(fd, args, num_args);
}

}  // namespace aidl::android::hardware::audio::core::mainline
