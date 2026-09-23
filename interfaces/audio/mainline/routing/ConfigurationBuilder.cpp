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

// Intersection of the capabilities of a set of endpoints, restricted to a
// channel count window.
alsa::HwCapabilities IntersectCapabilities(const std::vector<const Endpoint*>& endpoints,
                                           unsigned int min_channels, unsigned int max_channels) {
    alsa::HwCapabilities caps;
    caps.min_channels = min_channels;
    caps.max_channels = max_channels;
    for (const Endpoint* e : endpoints) {
        caps.min_channels = std::max(caps.min_channels, e->caps.min_channels);
        caps.max_channels = std::min(caps.max_channels, e->caps.max_channels);
    }

    if (endpoints.empty() || endpoints.front()->caps.formats.empty() ||
        endpoints.front()->caps.rates.empty())
        return caps;

    caps.formats = endpoints.front()->caps.formats;
    caps.rates = endpoints.front()->caps.rates;

    for (size_t i = 1; i < endpoints.size(); ++i) {
        std::erase_if(caps.formats,
                      [&](const auto& f) { return endpoints[i]->caps.formats.count(f) == 0; });
        std::erase_if(caps.rates,
                      [&](const auto& r) { return endpoints[i]->caps.rates.count(r) == 0; });
    }

    return caps;
}

bool IsHraFormat(snd_pcm_format_t format) {
    return format == SND_PCM_FORMAT_S24_LE || format == SND_PCM_FORMAT_S24_3LE ||
           format == SND_PCM_FORMAT_S32_LE || format == SND_PCM_FORMAT_FLOAT_LE;
}

bool IsHraRate(unsigned int rate) {
    return rate >= kHraOutputCutoff;
}

// Whether `caps` still describes at least one configuration. A mix port
// without profiles is not an error to Module and the framework but a
// *dynamic* port, whose profiles are expected to be filled in at connection
// time, which never happens for ours.
bool HasCommonProfile(const alsa::HwCapabilities& caps) {
    return !caps.formats.empty() && !caps.rates.empty() && caps.max_channels > 0 &&
           caps.min_channels <= caps.max_channels;
}

// Removes the elements of `set` matching `pred`, unless that would leave it
// empty.
template <typename Set, typename Pred>
void EraseIfSomethingRemains(Set& set, Pred pred) {
    Set kept = set;
    std::erase_if(kept, pred);
    if (!kept.empty()) set = std::move(kept);
}

// Splits high resolution audio off the primary output. The "hra output" keeps
// only high resolution formats and rates. The primary output drops them, but
// only as long as something remains: when every format (or rate) the device
// ports share is a high resolution one, e.g. because the card rates / bits
// properties asked for exactly that, the primary output keeps them rather
// than end up without any profile.
alsa::HwCapabilities HraFilter(alsa::HwCapabilities caps, bool is_hra) {
    if (is_hra) {
        std::erase_if(caps.formats, [](const auto& f) { return !IsHraFormat(f); });
        std::erase_if(caps.rates, [](const auto& r) { return !IsHraRate(r); });
    } else {
        EraseIfSomethingRemains(caps.formats, [](const auto& f) { return IsHraFormat(f); });
        EraseIfSomethingRemains(caps.rates, [](const auto& r) { return IsHraRate(r); });
    }

    return caps;
}

// The primary ports must always have profiles. When the device ports routed
// to one have nothing in common (possible once the card rates / bits
// properties have restricted them), fall back to what the plug layer can
// always serve.
alsa::HwCapabilities OrFallback(alsa::HwCapabilities caps, const char* port_name, bool is_input,
                                unsigned int min_channels, unsigned int max_channels) {
    if (HasCommonProfile(caps)) return caps;
    LOG(WARNING) << __func__ << ": the device ports of \"" << port_name
                 << "\" have no configuration in common (" << caps.ToString()
                 << "), using the fallback profile";
    alsa::HwCapabilities fallback = alsa::FallbackCapabilities(is_input);
    fallback.min_channels = min_channels;
    fallback.max_channels = max_channels;
    return fallback;
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
    std::vector<int32_t> hires_device_ports;
    std::vector<int32_t> multichannel_device_ports;
    std::vector<const Endpoint*> output_endpoints;
    std::vector<const Endpoint*> input_endpoints;
    std::vector<const Endpoint*> hires_endpoints;
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
            if (!endpoint.IsNull() && std::ranges::any_of(endpoint.caps.rates, IsHraRate) &&
                std::ranges::any_of(endpoint.caps.formats, IsHraFormat)) {
                hires_device_ports.push_back(port.id);
                hires_endpoints.push_back(&endpoint);
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
            alsa::ProfilesFromCapabilities(
                    OrFallback(HraFilter(IntersectCapabilities(output_endpoints, 1, 2), false),
                               kPrimaryOutputMixPort, false, 1, 2),
                    false));
    for (const int32_t sink : output_device_ports) {
        c->routes.push_back(MakeRoute({primary_out.id}, sink));
    }
    c->ports.push_back(std::move(primary_out));

    // The optional outputs are left out when their device ports have nothing
    // in common, rather than exposed without profiles.
    auto add_optional_output = [&c](const char* name, int32_t flags,
                                    const alsa::HwCapabilities& caps,
                                    const std::vector<int32_t>& sinks) {
        if (!HasCommonProfile(caps)) {
            LOG(WARNING) << "BuildConfiguration: not exposing \"" << name << "\": its "
                         << sinks.size() << " device port(s) have no configuration in common";
            return;
        }
        AudioPort port = MakeMixPort(c->nextPortId++, name, false, flags, 1, 1,
                                     alsa::ProfilesFromCapabilities(caps, false));
        LOG(INFO) << "BuildConfiguration: exposing \"" << name << "\" for " << sinks.size()
                  << " device port(s): " << caps.ToString();
        for (const int32_t sink : sinks) {
            c->routes.push_back(MakeRoute({port.id}, sink));
        }
        c->ports.push_back(std::move(port));
    };

    if (properties.multichannel && !multichannel_endpoints.empty()) {
        add_optional_output(
                kMultichannelOutputMixPort, makeBitPositionFlagMask(AudioOutputFlags::DIRECT),
                IntersectCapabilities(multichannel_endpoints, 3, 8), multichannel_device_ports);
    }

    if (!hires_endpoints.empty()) {
        add_optional_output(kHiresOutputMixPort,
                            makeBitPositionFlagMask(AudioOutputFlags::DIRECT) |
                                    makeBitPositionFlagMask(AudioOutputFlags::DIRECT_PCM),
                            HraFilter(IntersectCapabilities(hires_endpoints, 1, 2), true),
                            hires_device_ports);
    }

    AudioPort primary_in = MakeMixPort(
            c->nextPortId++, kPrimaryInputMixPort, true, 0, 0, 1,
            alsa::ProfilesFromCapabilities(OrFallback(IntersectCapabilities(input_endpoints, 1, 2),
                                                      kPrimaryInputMixPort, true, 1, 2),
                                           true));
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
