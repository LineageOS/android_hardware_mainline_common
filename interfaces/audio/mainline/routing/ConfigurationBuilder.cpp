/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_ConfigBuilder"

#include "routing/ConfigurationBuilder.h"

#include <algorithm>
#include <optional>

#include <Utils.h>
#include <aidl/android/media/audio/common/AudioDeviceDescription.h>
#include <aidl/android/media/audio/common/AudioDeviceType.h>
#include <aidl/android/media/audio/common/AudioOutputFlags.h>
#include <aidl/android/media/audio/common/AudioPortDeviceExt.h>
#include <aidl/android/media/audio/common/AudioPortMixExt.h>
#include <android-base/logging.h>

#include "alsa/AlsaFormat.h"

namespace aidl::android::hardware::audio::core::mainline::routing {

using ::aidl::android::hardware::audio::common::makeBitPositionFlagMask;
using ::aidl::android::media::audio::common::AudioChannelLayout;
using ::aidl::android::media::audio::common::AudioDeviceDescription;
using ::aidl::android::media::audio::common::AudioDeviceType;
using ::aidl::android::media::audio::common::AudioFormatDescription;
using ::aidl::android::media::audio::common::AudioIoFlags;
using ::aidl::android::media::audio::common::AudioOutputFlags;
using ::aidl::android::media::audio::common::AudioPort;
using ::aidl::android::media::audio::common::AudioPortConfig;
using ::aidl::android::media::audio::common::AudioPortDeviceExt;
using ::aidl::android::media::audio::common::AudioPortExt;
using ::aidl::android::media::audio::common::AudioPortMixExt;
using ::aidl::android::media::audio::common::AudioProfile;
using ::aidl::android::media::audio::common::Int;
using Configuration = Module::Configuration;

namespace {

AudioIoFlags MakeFlags(bool is_input, int32_t flags) {
    return is_input ? AudioIoFlags::make<AudioIoFlags::Tag::input>(flags)
                    : AudioIoFlags::make<AudioIoFlags::Tag::output>(flags);
}

AudioPort MakeDevicePort(int32_t id, const Endpoint& endpoint) {
    AudioPortDeviceExt ext;
    ext.device = endpoint.device;
    ext.flags = endpoint.is_default ? 1 << AudioPortDeviceExt::FLAG_INDEX_DEFAULT_DEVICE : 0;
    AudioPort port;
    port.id = id;
    port.name = endpoint.name;
    port.flags = MakeFlags(endpoint.is_input, 0);
    port.profiles = endpoint.profiles;
    port.ext = AudioPortExt::make<AudioPortExt::Tag::device>(ext);
    return port;
}

AudioPort MakeUsbTemplatePort(int32_t id, const std::string& name, AudioDeviceType type,
                              bool is_input) {
    AudioPortDeviceExt ext;
    ext.device.type.type = type;
    ext.device.type.connection = AudioDeviceDescription::CONNECTION_USB;
    AudioPort port;
    port.id = id;
    port.name = name;
    port.flags = MakeFlags(is_input, 0);
    port.ext = AudioPortExt::make<AudioPortExt::Tag::device>(ext);
    return port;
}

AudioPort MakeMixPort(int32_t id, const std::string& name, bool is_input, int32_t flags,
                      int32_t max_open, int32_t max_active, std::vector<AudioProfile> profiles) {
    AudioPortMixExt ext;
    ext.maxOpenStreamCount = max_open;
    ext.maxActiveStreamCount = max_active;
    AudioPort port;
    port.id = id;
    port.name = name;
    port.flags = MakeFlags(is_input, flags);
    port.profiles = std::move(profiles);
    port.ext = AudioPortExt::make<AudioPortExt::Tag::mix>(ext);
    return port;
}

// A port config with everything left dynamic, mirroring what the framework
// sees before it configures the port.
//
// `gain` is deliberately left null. The framework (Hal2AidlMapper) takes an
// existing device port config as the template for its own requests, so any
// gain we put here is echoed back in setAudioPortConfig(); Module then
// validates it against the port's gain controllers and, as none of our ports
// declare any, rejects the request and the stream open fails.
AudioPortConfig MakeDynamicPortConfig(const AudioPort& port) {
    AudioPortConfig config;
    config.id = port.id;
    config.portId = port.id;
    config.format = AudioFormatDescription{};
    config.channelMask = AudioChannelLayout{};
    config.sampleRate = Int{.value = 0};
    config.gain = std::nullopt;
    config.flags = port.flags;
    config.ext = port.ext;
    if (config.ext.getTag() == AudioPortExt::Tag::device) {
        // Configs do not carry the default-device flag.
        config.ext.get<AudioPortExt::Tag::device>().flags = 0;
    }
    return config;
}

AudioRoute MakeRoute(const std::vector<int32_t>& sources, int32_t sink) {
    AudioRoute route;
    route.sourcePortIds = sources;
    route.sinkPortId = sink;
    return route;
}

// Union of the capabilities of a set of endpoints, restricted to a channel
// count window.
alsa::HwCapabilities UnionCapabilities(const std::vector<const Endpoint*>& endpoints,
                                       unsigned int min_channels, unsigned int max_channels) {
    alsa::HwCapabilities caps;
    for (const Endpoint* e : endpoints) {
        caps.formats.insert(e->caps.formats.begin(), e->caps.formats.end());
        caps.rates.insert(e->caps.rates.begin(), e->caps.rates.end());
    }
    caps.min_channels = min_channels;
    caps.max_channels = max_channels;
    return caps;
}

// USB template ports get a fixed set of "connected" profiles for the
// connection simulation mode of the module (ModuleDebug).
std::vector<AudioProfile> UsbSimulationProfiles() {
    alsa::HwCapabilities caps;
    caps.formats = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_3LE};
    caps.rates = {44100, 48000};
    caps.min_channels = 1;
    caps.max_channels = 2;
    return alsa::ProfilesFromCapabilities(caps, false);
}

}  // namespace

std::unique_ptr<Configuration> BuildConfiguration(DeviceInventory& inventory,
                                                  const Properties& properties) {
    auto c = std::make_unique<Configuration>();

    // --- Device ports, one per endpoint -------------------------------------
    std::vector<int32_t> output_device_ports;
    std::vector<int32_t> input_device_ports;
    std::vector<int32_t> multichannel_device_ports;
    std::vector<const Endpoint*> output_endpoints;
    std::vector<const Endpoint*> input_endpoints;
    std::vector<const Endpoint*> multichannel_endpoints;

    for (Endpoint& endpoint : inventory.mutable_endpoints()) {
        endpoint.port_id = c->nextPortId++;
        AudioPort port = MakeDevicePort(endpoint.port_id, endpoint);
        c->initialConfigs.push_back(MakeDynamicPortConfig(port));
        if (!endpoint.IsAttached()) {
            // Template: profiles are only revealed once the device connects.
            c->connectedProfiles[port.id] = port.profiles;
            port.profiles.clear();
        }
        if (endpoint.is_input) {
            input_device_ports.push_back(port.id);
            input_endpoints.push_back(&endpoint);
        } else {
            output_device_ports.push_back(port.id);
            output_endpoints.push_back(&endpoint);
            if (endpoint.caps.max_channels >= 6 && !endpoint.IsNull()) {
                multichannel_device_ports.push_back(port.id);
                multichannel_endpoints.push_back(&endpoint);
            }
        }
        LOG(INFO) << __func__ << ": device port " << port.id << " <- " << endpoint.ToString();
        c->ports.push_back(std::move(port));
    }

    // --- USB device port templates -----------------------------------------
    struct UsbTemplate {
        const char* name;
        AudioDeviceType type;
        bool is_input;
    };
    static constexpr UsbTemplate kUsbTemplates[] = {
            {"USB Device Out", AudioDeviceType::OUT_DEVICE, false},
            {"USB Headset Out", AudioDeviceType::OUT_HEADSET, false},
            {"USB Device In", AudioDeviceType::IN_DEVICE, true},
            {"USB Headset In", AudioDeviceType::IN_HEADSET, true},
    };
    std::vector<int32_t> usb_output_ports;
    std::vector<int32_t> usb_input_ports;
    const std::vector<AudioProfile> usb_profiles = UsbSimulationProfiles();
    for (const auto& tmpl : kUsbTemplates) {
        AudioPort port = MakeUsbTemplatePort(c->nextPortId++, tmpl.name, tmpl.type, tmpl.is_input);
        c->connectedProfiles[port.id] = usb_profiles;
        c->initialConfigs.push_back(MakeDynamicPortConfig(port));
        (tmpl.is_input ? usb_input_ports : usb_output_ports).push_back(port.id);
        LOG(INFO) << __func__ << ": USB template port " << port.id << " \"" << tmpl.name << "\"";
        c->ports.push_back(std::move(port));
    }

    // --- Mix ports ----------------------------------------------------------
    AudioPort primary_out = MakeMixPort(
            c->nextPortId++, kPrimaryOutputMixPort, false,
            makeBitPositionFlagMask(AudioOutputFlags::PRIMARY), 1, 1,
            alsa::ProfilesFromCapabilities(UnionCapabilities(output_endpoints, 1, 2), false));
    for (const int32_t sink : output_device_ports) {
        c->routes.push_back(MakeRoute({primary_out.id}, sink));
    }
    c->ports.push_back(std::move(primary_out));

    if (properties.multichannel && !multichannel_endpoints.empty()) {
        AudioPort multichannel_out =
                MakeMixPort(c->nextPortId++, kMultichannelOutputMixPort, false,
                            makeBitPositionFlagMask(AudioOutputFlags::DIRECT), 1, 1,
                            alsa::ProfilesFromCapabilities(
                                    UnionCapabilities(multichannel_endpoints, 3, 8), false));
        LOG(INFO) << __func__ << ": exposing \"" << kMultichannelOutputMixPort << "\" for "
                  << multichannel_endpoints.size() << " device port(s)";
        for (const int32_t sink : multichannel_device_ports) {
            c->routes.push_back(MakeRoute({multichannel_out.id}, sink));
        }
        c->ports.push_back(std::move(multichannel_out));
    }

    AudioPort primary_in = MakeMixPort(
            c->nextPortId++, kPrimaryInputMixPort, true, 0, 0, 1,
            alsa::ProfilesFromCapabilities(UnionCapabilities(input_endpoints, 1, 2), true));
    c->routes.push_back(MakeRoute(input_device_ports, primary_in.id));
    c->ports.push_back(std::move(primary_in));

    // USB mix ports have dynamic profiles, filled in by the base Module when a
    // USB device connects.
    AudioPort usb_out = MakeMixPort(c->nextPortId++, kUsbOutputMixPort, false, 0, 1, 1, {});
    for (const int32_t sink : usb_output_ports) {
        c->routes.push_back(MakeRoute({usb_out.id}, sink));
    }
    c->ports.push_back(std::move(usb_out));

    AudioPort usb_in = MakeMixPort(c->nextPortId++, kUsbInputMixPort, true, 0, 0, 1, {});
    c->routes.push_back(MakeRoute(usb_input_ports, usb_in.id));
    c->ports.push_back(std::move(usb_in));

    c->portConfigs.insert(c->portConfigs.end(), c->initialConfigs.begin(), c->initialConfigs.end());

    LOG(INFO) << __func__ << ": " << c->ports.size() << " port(s), " << c->routes.size()
              << " route(s)";
    return c;
}

}  // namespace aidl::android::hardware::audio::core::mainline::routing
