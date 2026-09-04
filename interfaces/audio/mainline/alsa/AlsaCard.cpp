/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_AlsaCard"

#include "alsa/AlsaCard.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <initializer_list>
#include <sstream>

#include <alsa/asoundlib.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "alsa/AlsaError.h"

namespace aidl::android::hardware::audio::core::mainline::alsa {

namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

bool ContainsAny(const std::string& haystack, std::initializer_list<const char*> needles) {
    const std::string lower = ToLower(haystack);
    return std::any_of(needles.begin(), needles.end(), [&lower](const char* needle) {
        return lower.find(needle) != std::string::npos;
    });
}

// Fills the playback / capture flags of `info` for the given device.
void QueryPcmDevice(snd_ctl_t* ctl, snd_pcm_info_t* pcm_info, int device, PcmDeviceInfo* info) {
    for (const snd_pcm_stream_t stream : {SND_PCM_STREAM_PLAYBACK, SND_PCM_STREAM_CAPTURE}) {
        snd_pcm_info_set_device(pcm_info, device);
        snd_pcm_info_set_subdevice(pcm_info, 0);
        snd_pcm_info_set_stream(pcm_info, stream);
        const int err = snd_ctl_pcm_info(ctl, pcm_info);
        if (err < 0) {
            // -ENOENT simply means the device has no such direction.
            if (err != -ENOENT) {
                LOG(DEBUG) << __func__ << ": snd_ctl_pcm_info(" << device << ", "
                           << snd_pcm_stream_name(stream) << "): " << ErrorString(err);
            }
            continue;
        }
        if (stream == SND_PCM_STREAM_PLAYBACK) {
            info->playback = true;
        } else {
            info->capture = true;
        }
        if (info->id.empty()) info->id = snd_pcm_info_get_id(pcm_info);
        if (info->name.empty()) info->name = snd_pcm_info_get_name(pcm_info);
    }
}

}  // namespace

// --- PcmDeviceInfo ---------------------------------------------------------

std::string PcmDeviceInfo::HwName() const {
    return "hw:" + std::to_string(card) + "," + std::to_string(device);
}

bool PcmDeviceInfo::LooksLikeHdmi() const {
    return ContainsAny(name, {"hdmi", "displayport", "/dp"}) || ContainsAny(id, {"hdmi"});
}

bool PcmDeviceInfo::LooksLikeSpdif() const {
    return ContainsAny(name, {"iec958", "spdif", "s/pdif", "digital"});
}

bool PcmDeviceInfo::LooksLikeAnalog() const {
    return !LooksLikeHdmi() && !LooksLikeSpdif();
}

std::string PcmDeviceInfo::ToString() const {
    std::ostringstream os;
    os << HwName() << " \"" << name << "\"" << (playback ? " playback" : "")
       << (capture ? " capture" : "");
    return os.str();
}

// --- CardInfo ----------------------------------------------------------------

std::string CardInfo::CtlName() const {
    return "hw:" + std::to_string(index);
}

bool CardInfo::IsUsb() const {
    return driver == "USB-Audio";
}

bool CardInfo::HasPlayback() const {
    return std::any_of(pcms.begin(), pcms.end(), [](const auto& p) { return p.playback; });
}

bool CardInfo::HasCapture() const {
    return std::any_of(pcms.begin(), pcms.end(), [](const auto& p) { return p.capture; });
}

bool CardInfo::HasAnalogPlayback() const {
    return std::any_of(pcms.begin(), pcms.end(),
                       [](const auto& p) { return p.playback && p.LooksLikeAnalog(); });
}

bool CardInfo::Matches(const std::string& selector) const {
    if (selector.empty()) return false;
    int selected_index = -1;
    if (::android::base::ParseInt(selector, &selected_index)) {
        return selected_index == index;
    }
    const std::string wanted = ToLower(selector);
    return wanted == ToLower(id) || wanted == ToLower(name) || wanted == ToLower(long_name);
}

std::string CardInfo::ToString() const {
    std::ostringstream os;
    os << "card " << index << " [" << id << "] driver=\"" << driver << "\" name=\"" << name
       << "\" longname=\"" << long_name << "\" pcms={";
    for (size_t i = 0; i < pcms.size(); ++i) {
        if (i != 0) os << ", ";
        os << pcms[i].ToString();
    }
    os << "}";
    return os.str();
}

// --- Enumeration -------------------------------------------------------------

std::optional<CardInfo> QueryCard(int index) {
    CardInfo card;
    card.index = index;

    snd_ctl_t* ctl = nullptr;
    if (const int err = snd_ctl_open(&ctl, card.CtlName().c_str(), SND_CTL_NONBLOCK); err < 0) {
        LOG(WARNING) << __func__ << ": snd_ctl_open(" << card.CtlName()
                     << ") failed: " << ErrorString(err);
        return std::nullopt;
    }
    CtlHandle ctl_handle(ctl);

    CardInfoPtr card_info = AllocCardInfo();
    if (const int err = snd_ctl_card_info(ctl, card_info.get()); err < 0) {
        LOG(WARNING) << __func__ << ": snd_ctl_card_info(" << card.CtlName()
                     << ") failed: " << ErrorString(err);
        return std::nullopt;
    }
    card.id = snd_ctl_card_info_get_id(card_info.get());
    card.driver = snd_ctl_card_info_get_driver(card_info.get());
    card.name = snd_ctl_card_info_get_name(card_info.get());
    card.long_name = snd_ctl_card_info_get_longname(card_info.get());
    card.mixer_name = snd_ctl_card_info_get_mixername(card_info.get());
    card.components = snd_ctl_card_info_get_components(card_info.get());

    PcmInfoPtr pcm_info = AllocPcmInfo();
    int device = -1;
    while (true) {
        if (const int err = snd_ctl_pcm_next_device(ctl, &device); err < 0) {
            LOG(WARNING) << __func__ << ": snd_ctl_pcm_next_device(" << card.CtlName()
                         << ") failed: " << ErrorString(err);
            break;
        }
        if (device < 0) break;
        PcmDeviceInfo info;
        info.card = index;
        info.device = device;
        QueryPcmDevice(ctl, pcm_info.get(), device, &info);
        if (info.playback || info.capture) {
            card.pcms.push_back(std::move(info));
        }
    }

    LOG(INFO) << __func__ << ": " << card.ToString();
    return card;
}

std::vector<CardInfo> EnumerateCards() {
    std::vector<CardInfo> cards;
    int index = -1;
    while (true) {
        if (const int err = snd_card_next(&index); err < 0) {
            LOG(ERROR) << __func__ << ": snd_card_next failed: " << ErrorString(err);
            break;
        }
        if (index < 0) break;
        if (auto card = QueryCard(index); card.has_value()) {
            cards.push_back(std::move(*card));
        }
    }
    LOG(INFO) << __func__ << ": found " << cards.size() << " sound card(s)";
    return cards;
}

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
