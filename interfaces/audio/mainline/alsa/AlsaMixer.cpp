/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_AlsaMixer"

#include "alsa/AlsaMixer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <string_view>

#include <alsa/asoundlib.h>
#include <android-base/logging.h>

#include "alsa/AlsaError.h"

namespace aidl::android::hardware::audio::core::mainline::alsa {

namespace {

std::string ToLower(std::string_view value) {
    std::string lower(value);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}

bool ContainsAny(const std::string& lower, std::initializer_list<const char*> needles) {
    return std::any_of(needles.begin(), needles.end(), [&lower](const char* needle) {
        return lower.find(needle) != std::string::npos;
    });
}

// Controls that must not be touched by a generic initialisation.
bool IsBlacklisted(const std::string& lower_name) {
    return ContainsAny(lower_name, {"beep", "loopback", "boost", "analog loopback", "sidetone",
                                    "auto-mute", "auto gain", "agc", "iec958", "spdif", "jack"});
}

// Opens the simple mixer of a card. Returns nullptr on failure.
MixerHandle OpenMixer(int card) {
    snd_mixer_t* raw = nullptr;
    int err = snd_mixer_open(&raw, 0);
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_mixer_open: " << ErrorString(err);
        return nullptr;
    }
    MixerHandle mixer(raw);
    const std::string name = "hw:" + std::to_string(card);
    if ((err = snd_mixer_attach(raw, name.c_str())) < 0) {
        LOG(WARNING) << __func__ << ": snd_mixer_attach(" << name << "): " << ErrorString(err);
        return nullptr;
    }
    if ((err = snd_mixer_selem_register(raw, nullptr, nullptr)) < 0) {
        LOG(WARNING) << __func__ << ": snd_mixer_selem_register: " << ErrorString(err);
        return nullptr;
    }
    if ((err = snd_mixer_load(raw)) < 0) {
        LOG(WARNING) << __func__ << ": snd_mixer_load(" << name << "): " << ErrorString(err);
        return nullptr;
    }
    return mixer;
}

long PercentToValue(long min, long max, int percent) {
    if (max <= min) return max;
    return min + std::lround((max - min) * (std::clamp(percent, 0, 100) / 100.0));
}

void InitPlaybackElement(snd_mixer_elem_t* elem, const std::string& name, int percent) {
    if (snd_mixer_selem_has_playback_switch(elem)) {
        if (const int err = snd_mixer_selem_set_playback_switch_all(elem, 1); err < 0) {
            LOG(DEBUG) << "  '" << name << "' playback switch on: " << ErrorString(err);
        } else {
            LOG(DEBUG) << "  '" << name << "' playback switch on";
        }
    }
    if (snd_mixer_selem_has_playback_volume(elem)) {
        long min = 0, max = 0;
        if (snd_mixer_selem_get_playback_volume_range(elem, &min, &max) == 0) {
            const long value = PercentToValue(min, max, percent);
            if (const int err = snd_mixer_selem_set_playback_volume_all(elem, value); err < 0) {
                LOG(DEBUG) << "  '" << name << "' playback volume " << value << ": "
                           << ErrorString(err);
            } else {
                LOG(DEBUG) << "  '" << name << "' playback volume " << value << " [" << min << ".."
                           << max << "]";
            }
        }
    }
}

void InitCaptureElement(snd_mixer_elem_t* elem, const std::string& name, int percent) {
    if (snd_mixer_selem_has_capture_switch(elem)) {
        if (const int err = snd_mixer_selem_set_capture_switch_all(elem, 1); err < 0) {
            LOG(DEBUG) << "  '" << name << "' capture switch on: " << ErrorString(err);
        } else {
            LOG(DEBUG) << "  '" << name << "' capture switch on";
        }
    }
    if (snd_mixer_selem_has_capture_volume(elem)) {
        long min = 0, max = 0;
        if (snd_mixer_selem_get_capture_volume_range(elem, &min, &max) == 0) {
            const long value = PercentToValue(min, max, percent);
            if (const int err = snd_mixer_selem_set_capture_volume_all(elem, value); err < 0) {
                LOG(DEBUG) << "  '" << name << "' capture volume " << value << ": "
                           << ErrorString(err);
            } else {
                LOG(DEBUG) << "  '" << name << "' capture volume " << value << " [" << min << ".."
                           << max << "]";
            }
        }
    }
}

}  // namespace

bool InitializeMixer(int card, const MixerInitOptions& options) {
    MixerHandle mixer = OpenMixer(card);
    if (mixer == nullptr) return false;

    LOG(INFO) << __func__ << ": card " << card << " playback=" << options.playback_percent
              << "% capture=" << options.capture_percent << "%";
    for (snd_mixer_elem_t* elem = snd_mixer_first_elem(mixer.get()); elem != nullptr;
         elem = snd_mixer_elem_next(elem)) {
        if (!snd_mixer_selem_is_active(elem)) continue;
        const std::string name = snd_mixer_selem_get_name(elem);
        const std::string lower = ToLower(name);
        if (IsBlacklisted(lower)) {
            LOG(DEBUG) << "  '" << name << "' skipped";
            continue;
        }
        InitPlaybackElement(elem, name, options.playback_percent);
        InitCaptureElement(elem, name, options.capture_percent);
    }
    return true;
}

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
