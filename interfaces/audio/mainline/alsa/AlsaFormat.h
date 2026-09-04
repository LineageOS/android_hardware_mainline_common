/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <aidl/android/media/audio/common/AudioChannelLayout.h>
#include <aidl/android/media/audio/common/AudioFormatDescription.h>
#include <aidl/android/media/audio/common/AudioProfile.h>
#include <alsa/asoundlib.h>

namespace aidl::android::hardware::audio::core::mainline::alsa {

// What a PCM device is able to do, as reported by snd_pcm_hw_params_any().
struct HwCapabilities {
    std::set<snd_pcm_format_t> formats;
    std::set<unsigned int> rates;  // Only the "standard" rates are probed.
    unsigned int min_channels = 0;
    unsigned int max_channels = 0;

    bool IsEmpty() const { return formats.empty() || rates.empty() || max_channels == 0; }
    std::string ToString() const;
};

// Capabilities assumed for a device that could not be probed (e.g. because it
// was busy at start-up). The "plug" layer of alsa-lib makes these work on any
// hardware.
HwCapabilities FallbackCapabilities(bool is_input);

// AIDL PCM format <-> alsa-lib format. Only interleaved little endian formats
// are handled, which is what both sides use in practice.
std::optional<snd_pcm_format_t> ToAlsaFormat(
        const ::aidl::android::media::audio::common::AudioFormatDescription& format);
std::optional<::aidl::android::media::audio::common::AudioFormatDescription> ToAidlFormat(
        snd_pcm_format_t format);

// Channel mask <-> channel count.
unsigned int ChannelCount(const ::aidl::android::media::audio::common::AudioChannelLayout& layout);
// Returns every AIDL channel mask that the HAL exposes for a given channel
// count range. Positional layouts are used for the well known counts, index
// masks are added for the rest.
std::vector<::aidl::android::media::audio::common::AudioChannelLayout> ChannelMasksFor(
        unsigned int min_channels, unsigned int max_channels, bool is_input);

// Builds the AIDL profiles for a device with the given capabilities.
std::vector<::aidl::android::media::audio::common::AudioProfile> ProfilesFromCapabilities(
        const HwCapabilities& caps, bool is_input);

// Sample rates the HAL probes for and is willing to expose.
const std::vector<unsigned int>& StandardSampleRates();

// Applies a linear gain in place. Formats other than the ones handled by
// ToAlsaFormat() are left untouched.
void ApplyGain(void* buffer, size_t frames, unsigned int channels, snd_pcm_format_t format,
               float gain);

// Android orders the channels of positional multichannel layouts as
// FL FR FC LFE BL BR (SL SR), ALSA expects FL FR BL BR FC LFE (SL SR).
// Reorders `frames` interleaved frames in place when `channels` is 6 or 8 and
// the layout is a positional (not index) mask. Returns whether a reordering
// was done.
bool ReorderChannelsAndroidToAlsa(
        void* buffer, size_t frames, unsigned int channels, snd_pcm_format_t format,
        const ::aidl::android::media::audio::common::AudioChannelLayout& layout);
bool ReorderChannelsAlsaToAndroid(
        void* buffer, size_t frames, unsigned int channels, snd_pcm_format_t format,
        const ::aidl::android::media::audio::common::AudioChannelLayout& layout);

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
