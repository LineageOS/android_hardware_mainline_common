/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Inventory"

#include "routing/DeviceInventory.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <sstream>

#include <aidl/android/media/audio/common/AudioDeviceDescription.h>
#include <aidl/android/media/audio/common/AudioDeviceType.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "alsa/AlsaMixer.h"
#include "alsa/AlsaPcm.h"
#include "ucm/UcmDeviceMapper.h"

namespace aidl::android::hardware::audio::core::mainline::routing {

using ::aidl::android::media::audio::common::AudioDevice;
using ::aidl::android::media::audio::common::AudioDeviceAddress;
using ::aidl::android::media::audio::common::AudioDeviceDescription;
using ::aidl::android::media::audio::common::AudioDeviceType;

namespace {

constexpr const char* kBuiltInMicAddress = "bottom";

// Splits "hw:PCH,3" / "hw:0,3" into ("PCH", 3). Returns false for anything
// that is not a plain hw: name.
bool ParseHwName(const std::string& name, std::string* card, int* device) {
    if (!::android::base::StartsWith(name, "hw:")) return false;
    const std::string rest = name.substr(3);
    const size_t comma = rest.find(',');
    if (comma == std::string::npos) {
        *card = rest;
        *device = 0;
        return !rest.empty();
    }
    *card = rest.substr(0, comma);
    return ::android::base::ParseInt(rest.substr(comma + 1), device) && !card->empty();
}

// Checks that a PCM name from a UCM profile refers to a device that actually
// exists on `card` in the given direction. Names of other cards or non hw:
// names are trusted.
bool PcmExists(const alsa::CardInfo& card, const std::string& pcm_name, bool playback) {
    std::string card_ref;
    int device = 0;
    if (!ParseHwName(pcm_name, &card_ref, &device)) return true;
    if (card_ref != card.id && card_ref != std::to_string(card.index)) return true;
    return std::any_of(card.pcms.begin(), card.pcms.end(), [&](const alsa::PcmDeviceInfo& pcm) {
        return pcm.device == device && (playback ? pcm.playback : pcm.capture);
    });
}

AudioDevice MakeDevice(AudioDeviceType type, const std::string& connection = "",
                       const std::string& address = "") {
    AudioDevice device;
    device.type.type = type;
    device.type.connection = connection;
    if (!address.empty()) {
        device.address = AudioDeviceAddress::make<AudioDeviceAddress::id>(address);
    }
    return device;
}

std::string DefaultName(DeviceRole role) {
    switch (role) {
        case DeviceRole::kSpeaker:
            return "Speaker";
        case DeviceRole::kEarpiece:
            return "Earpiece";
        case DeviceRole::kHeadphones:
            return "Headphones";
        case DeviceRole::kHeadset:
            return "Headset";
        case DeviceRole::kLineOut:
            return "Line Out";
        case DeviceRole::kHdmi:
            return "HDMI";
        case DeviceRole::kSpdif:
            return "SPDIF";
        case DeviceRole::kMic:
            return "Built-In Mic";
        case DeviceRole::kHeadsetMic:
            return "Headset Mic";
        default:
            return "";
    }
}

// Endpoints that the plug layer of alsa-lib can always serve, regardless of
// what the hardware natively supports. Guarantees that the framework's
// favourite configuration (16-bit stereo 48 kHz) is always available.
void AugmentCapabilities(alsa::HwCapabilities* caps) {
    caps->formats.insert(SND_PCM_FORMAT_S16_LE);
    caps->rates.insert(44100);
    caps->rates.insert(48000);
    caps->min_channels = 1;
    caps->max_channels = std::max(caps->max_channels, 2u);
}

}  // namespace

// --- Endpoint ----------------------------------------------------------------

std::string Endpoint::ToString() const {
    std::ostringstream os;
    os << "\"" << name << "\" role=" << routing::ToString(role) << (is_input ? " in" : " out")
       << (is_default ? " default" : "") << (IsAttached() ? " attached" : " template")
       << " device=" << device.toString();
    if (IsNull()) {
        os << " backend=null";
    } else {
        os << " card=" << card << "[" << card_id << "] pcm=" << pcm_name;
        if (!ucm_device.empty()) os << " ucm=\"" << ucm_device << "\"";
        if (fixed_channels != 0) os << " channels=" << fixed_channels;
        os << " prio=" << priority << " caps={" << caps.ToString() << "}";
    }
    os << " profiles=" << profiles.size();
    if (port_id != 0) os << " portId=" << port_id;
    return os.str();
}

// --- DeviceInventory ---------------------------------------------------------

std::shared_ptr<DeviceInventory> DeviceInventory::Discover(const Properties& properties) {
    std::shared_ptr<DeviceInventory> inventory(new DeviceInventory());
    inventory->SelectCards(properties);
    inventory->CollectCandidates(properties);
    inventory->ProbeCapabilities();
    inventory->AssignRoles();
    inventory->AddNullEndpointsIfNeeded();
    inventory->FinalizeEndpoints();
    LOG(INFO) << inventory->Dump();
    return inventory;
}

void DeviceInventory::SelectCards(const Properties& properties) {
    std::vector<alsa::CardInfo> all = alsa::EnumerateCards();
    for (auto& card : all) {
        bool selected;
        if (!properties.cards.empty()) {
            selected = std::any_of(properties.cards.begin(), properties.cards.end(),
                                   [&card](const std::string& s) { return card.Matches(s); });
        } else {
            selected = !card.IsUsb() || properties.include_usb_cards;
        }
        if (!selected) {
            LOG(INFO) << __func__ << ": skipping " << card.ToString();
            continue;
        }
        cards_.push_back(std::move(card));
    }

    if (!properties.primary_card.empty()) {
        for (const auto& card : cards_) {
            if (card.Matches(properties.primary_card)) {
                primary_card_ = card.index;
                break;
            }
        }
        if (primary_card_ < 0) {
            LOG(WARNING) << __func__ << ": primary card \"" << properties.primary_card
                         << "\" not found among the selected cards";
        }
    }
    if (primary_card_ < 0) {
        // Prefer a card with an analog output, then any output, then any input.
        auto pick = [this](auto predicate) {
            const auto it = std::find_if(cards_.begin(), cards_.end(), predicate);
            if (it == cards_.end()) return false;
            primary_card_ = it->index;
            return true;
        };
        if (!pick([](const alsa::CardInfo& c) { return c.HasAnalogPlayback(); }) &&
            !pick([](const alsa::CardInfo& c) { return c.HasPlayback(); })) {
            pick([](const alsa::CardInfo& c) { return c.HasCapture(); });
        }
    }
    // Keep the primary card in front so that later tie-breaks favour it.
    std::stable_partition(cards_.begin(), cards_.end(),
                          [this](const alsa::CardInfo& c) { return c.index == primary_card_; });
    LOG(INFO) << __func__ << ": using " << cards_.size() << " card(s), primary card index "
              << primary_card_;
}

void DeviceInventory::CollectCandidates(const Properties& properties) {
    for (const auto& card : cards_) {
        std::unique_ptr<ucm::UcmManager> ucm;
        if (properties.ucm_enabled) {
            ucm = ucm::UcmManager::Open(card.index, properties.ucm_verb);
        }
        if (ucm != nullptr) {
            CollectFromUcm(card, *ucm);
            ucm_managers_[card.index] = std::move(ucm);
        } else {
            LOG(INFO) << __func__ << ": card " << card.index
                      << " has no UCM profile, using PCM device heuristics";
            if (properties.mixer_init) {
                alsa::MixerInitOptions options;
                options.playback_percent = properties.mixer_playback_percent;
                options.capture_percent = properties.mixer_capture_percent;
                alsa::InitializeMixer(card.index, options);
            }
            CollectFromPcmDevices(card);
        }
    }
}

void DeviceInventory::CollectFromUcm(const alsa::CardInfo& card, ucm::UcmManager& ucm) {
    for (const ucm::UcmDevice& device : ucm.devices()) {
        for (const bool playback : {true, false}) {
            const std::string& pcm = playback ? device.playback_pcm : device.capture_pcm;
            if (pcm.empty()) continue;
            if (!PcmExists(card, pcm, playback)) {
                LOG(WARNING) << __func__ << ": UCM device \"" << device.name << "\" refers to "
                             << pcm << " which does not exist on card " << card.index
                             << ", skipping";
                continue;
            }
            Endpoint endpoint;
            endpoint.role = ucm::ClassifyUcmDevice(device.name, playback);
            endpoint.is_input = !playback;
            endpoint.card = card.index;
            endpoint.card_id = card.id;
            endpoint.pcm_name = pcm;
            endpoint.ucm_device = device.name;
            endpoint.priority = playback ? device.playback_priority : device.capture_priority;
            endpoint.fixed_channels = static_cast<unsigned int>(playback ? device.playback_channels
                                                                         : device.capture_channels);
            endpoint.name = device.name;
            endpoints_.push_back(std::move(endpoint));
        }
    }
}

void DeviceInventory::CollectFromPcmDevices(const alsa::CardInfo& card) {
    bool first_analog_playback = true;
    bool first_analog_capture = true;
    for (const alsa::PcmDeviceInfo& pcm : card.pcms) {
        if (pcm.playback) {
            Endpoint endpoint;
            if (pcm.LooksLikeHdmi()) {
                endpoint.role = DeviceRole::kHdmi;
            } else if (pcm.LooksLikeSpdif()) {
                endpoint.role = DeviceRole::kSpdif;
            } else if (first_analog_playback) {
                endpoint.role = DeviceRole::kSpeaker;
                first_analog_playback = false;
            } else {
                endpoint.role = DeviceRole::kLineOut;
            }
            endpoint.is_input = false;
            endpoint.card = card.index;
            endpoint.card_id = card.id;
            endpoint.pcm_name = pcm.HwName();
            endpoint.name = pcm.name;
            // Lower device numbers first.
            endpoint.priority = 1000 - pcm.device;
            endpoints_.push_back(std::move(endpoint));
        }
        if (pcm.capture) {
            Endpoint endpoint;
            if (first_analog_capture && pcm.LooksLikeAnalog()) {
                endpoint.role = DeviceRole::kMic;
                first_analog_capture = false;
            } else {
                endpoint.role = DeviceRole::kBusIn;
            }
            endpoint.is_input = true;
            endpoint.card = card.index;
            endpoint.card_id = card.id;
            endpoint.pcm_name = pcm.HwName();
            endpoint.name = pcm.name;
            endpoint.priority = 1000 - pcm.device;
            endpoints_.push_back(std::move(endpoint));
        }
    }
}

void DeviceInventory::ProbeCapabilities() {
    for (Endpoint& endpoint : endpoints_) {
        const snd_pcm_stream_t stream =
                endpoint.is_input ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
        auto caps = alsa::QueryCapabilities(endpoint.pcm_name, stream);
        if (!caps.has_value()) {
            LOG(WARNING) << __func__ << ": could not probe " << endpoint.pcm_name
                         << ", assuming a basic PCM device";
            caps = alsa::FallbackCapabilities(endpoint.is_input);
        }
        if (endpoint.fixed_channels != 0) {
            caps->max_channels = std::min(caps->max_channels, endpoint.fixed_channels);
            if (caps->max_channels == 0) caps->max_channels = endpoint.fixed_channels;
            caps->min_channels = std::min(caps->min_channels, caps->max_channels);
        }
        AugmentCapabilities(&*caps);
        endpoint.caps = *caps;
    }
}

void DeviceInventory::AssignRoles() {
    // Primary card first, then higher UCM priority, otherwise keep the
    // discovery order.
    std::stable_sort(endpoints_.begin(), endpoints_.end(),
                     [this](const Endpoint& a, const Endpoint& b) {
                         const bool a_primary = a.card == primary_card_;
                         const bool b_primary = b.card == primary_card_;
                         if (a_primary != b_primary) return a_primary;
                         return a.priority > b.priority;
                     });

    bool have_speaker = false;
    bool have_earpiece = false;
    bool have_mic = false;
    std::set<DeviceRole> used_templates;

    for (Endpoint& e : endpoints_) {
        const bool on_primary = e.card == primary_card_;
        switch (e.role) {
            case DeviceRole::kSpeaker:
                if (on_primary && !have_speaker) {
                    have_speaker = true;
                } else {
                    e.role = DeviceRole::kBusOut;
                }
                break;
            case DeviceRole::kEarpiece:
                if (on_primary && !have_earpiece) {
                    have_earpiece = true;
                } else {
                    e.role = DeviceRole::kBusOut;
                }
                break;
            case DeviceRole::kMic:
                if (on_primary && !have_mic) {
                    have_mic = true;
                } else {
                    e.role = DeviceRole::kBusIn;
                }
                break;
            case DeviceRole::kHeadphones:
            case DeviceRole::kHeadset:
            case DeviceRole::kLineOut:
            case DeviceRole::kHdmi:
            case DeviceRole::kSpdif:
            case DeviceRole::kHeadsetMic:
                // The framework can only connect one external device per type,
                // so only the first template of each kind is useful.
                if (!used_templates.insert(e.role).second) {
                    e.role = e.is_input ? DeviceRole::kBusIn : DeviceRole::kBusOut;
                }
                break;
            case DeviceRole::kBusOut:
            case DeviceRole::kBusIn:
                break;
        }
    }

    // Every module needs an attached default output and input. Promote the
    // most suitable path when the card has no dedicated speaker / microphone,
    // e.g. desktop codecs with line out only or HDMI-only TV boxes.
    auto promote = [this, &used_templates](bool is_input, DeviceRole target,
                                           std::initializer_list<DeviceRole> preference) {
        for (const bool primary_only : {true, false}) {
            for (const DeviceRole wanted : preference) {
                for (Endpoint& e : endpoints_) {
                    if (e.is_input != is_input || e.role != wanted) continue;
                    if (primary_only && e.card != primary_card_) continue;
                    LOG(INFO) << __func__ << ": promoting \"" << e.name << "\" (" << e.pcm_name
                              << ", " << routing::ToString(e.role) << ") to "
                              << routing::ToString(target);
                    used_templates.erase(e.role);
                    e.role = target;
                    return true;
                }
            }
        }
        return false;
    };
    if (!have_speaker) {
        have_speaker = promote(false, DeviceRole::kSpeaker,
                               {DeviceRole::kLineOut, DeviceRole::kBusOut, DeviceRole::kHeadphones,
                                DeviceRole::kHeadset, DeviceRole::kHdmi, DeviceRole::kSpdif});
    }
    if (!have_mic) {
        have_mic = promote(true, DeviceRole::kMic, {DeviceRole::kBusIn, DeviceRole::kHeadsetMic});
    }
}

void DeviceInventory::AddNullEndpointsIfNeeded() {
    const bool have_speaker =
            std::any_of(endpoints_.begin(), endpoints_.end(),
                        [](const Endpoint& e) { return e.role == DeviceRole::kSpeaker; });
    const bool have_mic = std::any_of(endpoints_.begin(), endpoints_.end(),
                                      [](const Endpoint& e) { return e.role == DeviceRole::kMic; });
    auto add_null = [this](DeviceRole role, bool is_input) {
        LOG(WARNING) << __func__ << ": no hardware for " << routing::ToString(role)
                     << ", adding a null device";
        Endpoint e;
        e.role = role;
        e.is_input = is_input;
        e.backend = Endpoint::Backend::kNull;
        e.name = DefaultName(role);
        e.pcm_name = "null";
        e.caps = alsa::FallbackCapabilities(is_input);
        endpoints_.push_back(std::move(e));
    };
    if (!have_speaker) add_null(DeviceRole::kSpeaker, false);
    if (!have_mic) add_null(DeviceRole::kMic, true);
}

void DeviceInventory::FinalizeEndpoints() {
    for (Endpoint& e : endpoints_) {
        const std::string default_name = DefaultName(e.role);
        std::string bus_address;
        if (e.role == DeviceRole::kBusOut || e.role == DeviceRole::kBusIn) {
            // Addressed ports need a stable, unique address: card id plus the
            // UCM device name or the PCM name.
            const std::string leaf = e.ucm_device.empty() ? e.pcm_name : e.ucm_device;
            bus_address = e.card_id + "/" + leaf;
            e.name = e.card_id + ": " + (e.ucm_device.empty() ? e.name : e.ucm_device);
        } else if (!default_name.empty()) {
            e.name = default_name;
        }
        e.is_default = e.role == DeviceRole::kSpeaker || e.role == DeviceRole::kMic;

        switch (e.role) {
            case DeviceRole::kSpeaker:
                e.device = MakeDevice(AudioDeviceType::OUT_SPEAKER);
                break;
            case DeviceRole::kEarpiece:
                e.device = MakeDevice(AudioDeviceType::OUT_SPEAKER_EARPIECE);
                break;
            case DeviceRole::kHeadphones:
                e.device = MakeDevice(AudioDeviceType::OUT_HEADPHONE,
                                      AudioDeviceDescription::CONNECTION_ANALOG);
                break;
            case DeviceRole::kHeadset:
                e.device = MakeDevice(AudioDeviceType::OUT_HEADSET,
                                      AudioDeviceDescription::CONNECTION_ANALOG);
                break;
            case DeviceRole::kLineOut:
                e.device = MakeDevice(AudioDeviceType::OUT_DEVICE,
                                      AudioDeviceDescription::CONNECTION_ANALOG);
                break;
            case DeviceRole::kHdmi:
                e.device = MakeDevice(AudioDeviceType::OUT_DEVICE,
                                      AudioDeviceDescription::CONNECTION_HDMI);
                break;
            case DeviceRole::kSpdif:
                e.device = MakeDevice(AudioDeviceType::OUT_DEVICE,
                                      AudioDeviceDescription::CONNECTION_SPDIF);
                break;
            case DeviceRole::kBusOut:
                e.device = MakeDevice(AudioDeviceType::OUT_BUS, "", bus_address);
                break;
            case DeviceRole::kMic:
                e.device = MakeDevice(AudioDeviceType::IN_MICROPHONE, "", kBuiltInMicAddress);
                break;
            case DeviceRole::kHeadsetMic:
                e.device = MakeDevice(AudioDeviceType::IN_HEADSET,
                                      AudioDeviceDescription::CONNECTION_ANALOG);
                break;
            case DeviceRole::kBusIn:
                e.device = MakeDevice(AudioDeviceType::IN_BUS, "", bus_address);
                break;
        }
        e.profiles = alsa::ProfilesFromCapabilities(e.caps, e.is_input);
    }
}

const Endpoint* DeviceInventory::FindByDevice(const AudioDevice& device) const {
    for (const Endpoint& e : endpoints_) {
        if (e.device == device) return &e;
    }
    // Connected external device ports may carry an address supplied by the
    // framework that the template does not have; match on type in that case.
    for (const Endpoint& e : endpoints_) {
        if (!e.IsAttached() && e.device.type == device.type) return &e;
    }
    return nullptr;
}

const Endpoint* DeviceInventory::FindByPortId(int32_t port_id) const {
    for (const Endpoint& e : endpoints_) {
        if (e.port_id == port_id) return &e;
    }
    return nullptr;
}

std::optional<Endpoint> DeviceInventory::MakeUsbEndpoint(const AudioDevice& device,
                                                         bool is_input) const {
    if (device.address.getTag() != AudioDeviceAddress::alsa) {
        LOG(ERROR) << __func__ << ": not an ALSA address: " << device.toString();
        return std::nullopt;
    }
    const auto& alsa_address = device.address.get<AudioDeviceAddress::alsa>();
    if (alsa_address.size() != 2 || alsa_address[0] < 0 || alsa_address[1] < 0) {
        LOG(ERROR) << __func__ << ": malformed ALSA address: " << device.toString();
        return std::nullopt;
    }
    Endpoint e;
    e.role = is_input ? DeviceRole::kBusIn : DeviceRole::kBusOut;
    e.is_input = is_input;
    e.device = device;
    e.card = alsa_address[0];
    e.card_id = std::to_string(e.card);
    e.pcm_name = "hw:" + std::to_string(alsa_address[0]) + "," + std::to_string(alsa_address[1]);
    e.name = "USB " + e.pcm_name;
    const snd_pcm_stream_t stream = is_input ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
    auto caps = alsa::QueryCapabilities(e.pcm_name, stream);
    if (!caps.has_value()) {
        LOG(ERROR) << __func__ << ": USB device " << e.pcm_name << " can not be probed";
        return std::nullopt;
    }
    e.caps = *caps;
    e.profiles = alsa::ProfilesFromCapabilities(e.caps, is_input);
    LOG(INFO) << __func__ << ": " << e.ToString();
    return e;
}

ucm::UcmManager* DeviceInventory::UcmForCard(int card) const {
    const auto it = ucm_managers_.find(card);
    return it != ucm_managers_.end() ? it->second.get() : nullptr;
}

bool DeviceInventory::HasMultichannelOutput() const {
    return std::any_of(endpoints_.begin(), endpoints_.end(), [](const Endpoint& e) {
        return !e.is_input && !e.IsNull() && e.caps.max_channels >= 6;
    });
}

std::string DeviceInventory::Dump() const {
    std::ostringstream os;
    os << "DeviceInventory: " << cards_.size() << " card(s), primary card " << primary_card_ << ", "
       << endpoints_.size() << " endpoint(s)\n";
    for (const auto& card : cards_) {
        os << "  " << card.ToString() << (UcmForCard(card.index) != nullptr ? " [UCM]" : "")
           << "\n";
    }
    for (const auto& e : endpoints_) {
        os << "  " << e.ToString() << "\n";
    }
    return os.str();
}

}  // namespace aidl::android::hardware::audio::core::mainline::routing
