/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_AlsaFormat"

#include "alsa/AlsaFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core::mainline::alsa {

using ::aidl::android::media::audio::common::AudioChannelLayout;
using ::aidl::android::media::audio::common::AudioFormatDescription;
using ::aidl::android::media::audio::common::AudioFormatType;
using ::aidl::android::media::audio::common::AudioProfile;
using ::aidl::android::media::audio::common::PcmType;

namespace {

struct FormatPair {
    PcmType pcm;
    snd_pcm_format_t alsa;
};

// Order matters: this is also the preference order used when building
// profiles.
constexpr std::array<FormatPair, 6> kFormatPairs = {{
        {PcmType::INT_16_BIT, SND_PCM_FORMAT_S16_LE},
        {PcmType::FLOAT_32_BIT, SND_PCM_FORMAT_FLOAT_LE},
        {PcmType::INT_24_BIT, SND_PCM_FORMAT_S24_3LE},
        {PcmType::FIXED_Q_8_24, SND_PCM_FORMAT_S24_LE},
        {PcmType::INT_32_BIT, SND_PCM_FORMAT_S32_LE},
        {PcmType::UINT_8_BIT, SND_PCM_FORMAT_U8},
}};

AudioChannelLayout LayoutMask(int32_t mask) {
    return AudioChannelLayout::make<AudioChannelLayout::layoutMask>(mask);
}

AudioChannelLayout IndexMask(int32_t mask) {
    return AudioChannelLayout::make<AudioChannelLayout::indexMask>(mask);
}

// Positional output layouts by channel count. Only layouts that the framework
// commonly requests are listed.
std::vector<int32_t> OutputLayoutsFor(unsigned int channels) {
    switch (channels) {
        case 1:
            return {AudioChannelLayout::LAYOUT_MONO};
        case 2:
            return {AudioChannelLayout::LAYOUT_STEREO};
        case 3:
            return {AudioChannelLayout::LAYOUT_2POINT1};
        case 4:
            return {AudioChannelLayout::LAYOUT_QUAD};
        case 6:
            return {AudioChannelLayout::LAYOUT_5POINT1};
        case 8:
            return {AudioChannelLayout::LAYOUT_7POINT1};
        default:
            return {};
    }
}

std::vector<int32_t> InputLayoutsFor(unsigned int channels) {
    switch (channels) {
        case 1:
            return {AudioChannelLayout::LAYOUT_MONO};
        case 2:
            return {AudioChannelLayout::LAYOUT_STEREO};
        default:
            return {};
    }
}

// Interleaved permutation of Android position order to ALSA order.
// 6 channels: Android FL FR FC LFE BL BR -> ALSA FL FR BL BR FC LFE.
// 8 channels: Android FL FR FC LFE BL BR SL SR -> ALSA FL FR BL BR FC LFE SL SR.
constexpr std::array<int, 6> kAndroidToAlsa6 = {0, 1, 4, 5, 2, 3};
constexpr std::array<int, 8> kAndroidToAlsa8 = {0, 1, 4, 5, 2, 3, 6, 7};
// The permutations above are involutions (swapping pairs), so the reverse
// mapping is identical.

template <size_t N>
void Permute(uint8_t* frame, size_t sample_bytes, const std::array<int, N>& map) {
    uint8_t tmp[N * 8];  // sample_bytes <= 8
    for (size_t dst = 0; dst < N; ++dst) {
        std::memcpy(tmp + dst * sample_bytes, frame + map[dst] * sample_bytes, sample_bytes);
    }
    std::memcpy(frame, tmp, N * sample_bytes);
}

bool Reorder(void* buffer, size_t frames, unsigned int channels, snd_pcm_format_t format,
             const AudioChannelLayout& layout) {
    if (layout.getTag() != AudioChannelLayout::layoutMask) return false;
    const int32_t mask = layout.get<AudioChannelLayout::layoutMask>();
    // Only the two layouts whose ALSA counterpart has a different order.
    if (mask != AudioChannelLayout::LAYOUT_5POINT1 && mask != AudioChannelLayout::LAYOUT_7POINT1) {
        return false;
    }
    const int sample_bytes = snd_pcm_format_physical_width(format) / 8;
    if (sample_bytes <= 0 || sample_bytes > 8) return false;
    uint8_t* data = static_cast<uint8_t*>(buffer);
    const size_t frame_bytes = static_cast<size_t>(sample_bytes) * channels;
    if (channels == 6) {
        for (size_t f = 0; f < frames; ++f)
            Permute(data + f * frame_bytes, sample_bytes, kAndroidToAlsa6);
        return true;
    }
    if (channels == 8) {
        for (size_t f = 0; f < frames; ++f)
            Permute(data + f * frame_bytes, sample_bytes, kAndroidToAlsa8);
        return true;
    }
    return false;
}

template <typename T>
void ScaleSamples(T* samples, size_t count, float gain) {
    constexpr double kMin = std::numeric_limits<T>::min();
    constexpr double kMax = std::numeric_limits<T>::max();
    for (size_t i = 0; i < count; ++i) {
        const double scaled = std::round(static_cast<double>(samples[i]) * gain);
        samples[i] = static_cast<T>(std::clamp(scaled, kMin, kMax));
    }
}

// Packed signed 24-bit little endian.
void ScaleSamples24(uint8_t* data, size_t count, float gain) {
    constexpr double kMin = -(1 << 23);
    constexpr double kMax = (1 << 23) - 1;
    for (size_t i = 0; i < count; ++i) {
        uint8_t* p = data + i * 3;
        // Assemble in the top 24 bits of an unsigned word, then let the
        // arithmetic shift of the signed value do the sign extension.
        const uint32_t raw = static_cast<uint32_t>(p[2]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
                             static_cast<uint32_t>(p[0]) << 8;
        const int32_t value = static_cast<int32_t>(raw) >> 8;
        const int32_t scaled = static_cast<int32_t>(
                std::clamp(std::round(static_cast<double>(value) * gain), kMin, kMax));
        p[0] = static_cast<uint8_t>(scaled & 0xff);
        p[1] = static_cast<uint8_t>((scaled >> 8) & 0xff);
        p[2] = static_cast<uint8_t>((scaled >> 16) & 0xff);
    }
}

}  // namespace

// --- HwCapabilities ---------------------------------------------------------

std::string HwCapabilities::ToString() const {
    std::ostringstream os;
    os << "formats={";
    bool first = true;
    for (const auto format : formats) {
        os << (first ? "" : ",") << snd_pcm_format_name(format);
        first = false;
    }
    os << "} rates={";
    first = true;
    for (const auto rate : rates) {
        os << (first ? "" : ",") << rate;
        first = false;
    }
    os << "} channels=" << min_channels << ".." << max_channels;
    return os.str();
}

HwCapabilities FallbackCapabilities(bool /*is_input*/) {
    HwCapabilities caps;
    caps.formats = {SND_PCM_FORMAT_S16_LE};
    caps.rates = {44100, 48000};
    caps.min_channels = 1;
    caps.max_channels = 2;
    return caps;
}

// --- Formats -----------------------------------------------------------------

std::optional<snd_pcm_format_t> ToAlsaFormat(const AudioFormatDescription& format) {
    if (format.type != AudioFormatType::PCM) return std::nullopt;
    for (const auto& pair : kFormatPairs) {
        if (pair.pcm == format.pcm) return pair.alsa;
    }
    return std::nullopt;
}

std::optional<AudioFormatDescription> ToAidlFormat(snd_pcm_format_t format) {
    for (const auto& pair : kFormatPairs) {
        if (pair.alsa == format) {
            return AudioFormatDescription{.type = AudioFormatType::PCM, .pcm = pair.pcm};
        }
    }
    return std::nullopt;
}

// --- Channels ----------------------------------------------------------------

unsigned int ChannelCount(const AudioChannelLayout& layout) {
    switch (layout.getTag()) {
        case AudioChannelLayout::layoutMask:
            return static_cast<unsigned int>(
                    __builtin_popcount(layout.get<AudioChannelLayout::layoutMask>()));
        case AudioChannelLayout::indexMask:
            return static_cast<unsigned int>(
                    __builtin_popcount(layout.get<AudioChannelLayout::indexMask>()));
        case AudioChannelLayout::voiceMask:
            return static_cast<unsigned int>(
                    __builtin_popcount(layout.get<AudioChannelLayout::voiceMask>()));
        default:
            return 0;
    }
}

std::vector<AudioChannelLayout> ChannelMasksFor(unsigned int min_channels,
                                                unsigned int max_channels, bool is_input) {
    std::vector<AudioChannelLayout> masks;
    // Cap what we advertise: Android has no positional layouts above 8 and the
    // plug layer converts anything anyway.
    const unsigned int upper = std::min(max_channels, 8u);
    for (unsigned int channels = std::max(min_channels, 1u); channels <= upper; ++channels) {
        for (const int32_t layout :
             is_input ? InputLayoutsFor(channels) : OutputLayoutsFor(channels)) {
            masks.push_back(LayoutMask(layout));
        }
        masks.push_back(IndexMask((1 << channels) - 1));
    }
    return masks;
}

// --- Profiles ----------------------------------------------------------------

const std::vector<unsigned int>& StandardSampleRates() {
    static const std::vector<unsigned int> kRates = {8000,  11025, 12000,  16000, 22050,
                                                     24000, 32000, 44100,  48000, 64000,
                                                     88200, 96000, 176400, 192000};
    return kRates;
}

std::vector<AudioProfile> ProfilesFromCapabilities(const HwCapabilities& caps, bool is_input) {
    std::vector<AudioProfile> profiles;
    const std::vector<AudioChannelLayout> masks =
            ChannelMasksFor(caps.min_channels, caps.max_channels, is_input);
    std::vector<int32_t> rates(caps.rates.begin(), caps.rates.end());
    if (masks.empty() || rates.empty()) return profiles;

    for (const auto& pair : kFormatPairs) {
        if (caps.formats.count(pair.alsa) == 0) continue;
        AudioProfile profile;
        profile.format = AudioFormatDescription{.type = AudioFormatType::PCM, .pcm = pair.pcm};
        profile.channelMasks = masks;
        profile.sampleRates = rates;
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

// --- Sample processing -------------------------------------------------------

void ApplyGain(void* buffer, size_t frames, unsigned int channels, snd_pcm_format_t format,
               float gain) {
    if (gain == 1.0f || buffer == nullptr) return;
    const size_t samples = frames * channels;
    switch (format) {
        case SND_PCM_FORMAT_S16_LE:
            ScaleSamples(static_cast<int16_t*>(buffer), samples, gain);
            break;
        case SND_PCM_FORMAT_S32_LE:
        case SND_PCM_FORMAT_S24_LE:
            ScaleSamples(static_cast<int32_t*>(buffer), samples, gain);
            break;
        case SND_PCM_FORMAT_FLOAT_LE: {
            float* data = static_cast<float*>(buffer);
            for (size_t i = 0; i < samples; ++i) data[i] *= gain;
            break;
        }
        case SND_PCM_FORMAT_S24_3LE:
            ScaleSamples24(static_cast<uint8_t*>(buffer), samples, gain);
            break;
        default:
            // Unsigned 8-bit and anything exotic: gain is not applied.
            break;
    }
}

bool ReorderChannelsAndroidToAlsa(void* buffer, size_t frames, unsigned int channels,
                                  snd_pcm_format_t format, const AudioChannelLayout& layout) {
    return Reorder(buffer, frames, channels, format, layout);
}

bool ReorderChannelsAlsaToAndroid(void* buffer, size_t frames, unsigned int channels,
                                  snd_pcm_format_t format, const AudioChannelLayout& layout) {
    // The mapping is its own inverse.
    return Reorder(buffer, frames, channels, format, layout);
}

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
