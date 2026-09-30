/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Passthrough"

#include "passthrough/Format.h"

#include <iomanip>
#include <sstream>

#include <alsa/asoundef.h>
#include <media/stagefright/foundation/MediaDefs.h>

namespace aidl::android::hardware::audio::core::mainline::passthrough {

using ::aidl::android::media::audio::common::AudioFormatDescription;
using ::aidl::android::media::audio::common::AudioFormatType;
using ::aidl::android::media::audio::common::PcmType;

namespace {

struct FormatNames {
    EncodedFormat format;
    const char* name;
    const char* mime;
    audio_format_t audio_format;
};

const std::vector<FormatNames>& Names() {
    // MEDIA_MIMETYPE_* are the encodings AidlConversionCppNdk maps the legacy
    // audio_format_t values to.
    static const std::vector<FormatNames> kNames = {
            {EncodedFormat::kAc3, "ac3", ::android::MEDIA_MIMETYPE_AUDIO_AC3, AUDIO_FORMAT_AC3},
            {EncodedFormat::kEac3, "eac3", ::android::MEDIA_MIMETYPE_AUDIO_EAC3,
             AUDIO_FORMAT_E_AC3},
            {EncodedFormat::kEac3Joc, "eac3-joc", ::android::MEDIA_MIMETYPE_AUDIO_EAC3_JOC,
             AUDIO_FORMAT_E_AC3_JOC},
            {EncodedFormat::kDts, "dts", ::android::MEDIA_MIMETYPE_AUDIO_DTS, AUDIO_FORMAT_DTS},
            {EncodedFormat::kIec61937, "iec61937", ::android::MEDIA_MIMETYPE_AUDIO_IEC61937,
             AUDIO_FORMAT_DEFAULT},
    };
    return kNames;
}

const FormatNames& NamesOf(EncodedFormat format) {
    for (const auto& names : Names()) {
        if (names.format == format) return names;
    }
    return Names().back();  // Not reached: every enumerator is listed.
}

// Sample rates an HDMI sink accepts for two channel PCM framing.
constexpr uint32_t kIecRates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};

bool IsIecRate(uint32_t rate) {
    for (const uint32_t r : kIecRates) {
        if (r == rate) return true;
    }
    return false;
}

// High bit rate: 8 channels at 192 kHz carry a 768 kHz two channel stream.
constexpr uint32_t kHbrPcmRate = 192000;
constexpr unsigned int kHbrChannels = 8;
constexpr uint32_t kHbrStreamRate = 768000;

uint8_t Aes3RateCode(uint32_t stream_rate) {
    switch (stream_rate) {
        case 32000:
            return IEC958_AES3_CON_FS_32000;
        case 44100:
            return IEC958_AES3_CON_FS_44100;
        case 48000:
            return IEC958_AES3_CON_FS_48000;
        case 88200:
            return IEC958_AES3_CON_FS_88200;
        case 96000:
            return IEC958_AES3_CON_FS_96000;
        case 176400:
            return IEC958_AES3_CON_FS_176400;
        case 192000:
            return IEC958_AES3_CON_FS_192000;
        case kHbrStreamRate:
            return IEC958_AES3_CON_FS_768000;
        default:
            return IEC958_AES3_CON_FS_NOTID;
    }
}

std::optional<IecStream> TwoChannel(uint32_t content_rate, uint32_t multiplier) {
    const uint32_t pcm_rate = content_rate * multiplier;
    if (!IsIecRate(pcm_rate)) return std::nullopt;
    return IecStream{.pcm_rate = pcm_rate,
                     .pcm_channels = 2,
                     .hbr = false,
                     .rate_multiplier = multiplier,
                     .aes3_rate_code = Aes3RateCode(pcm_rate)};
}

}  // namespace

const std::vector<EncodedFormat>& AllEncodedFormats() {
    static const std::vector<EncodedFormat> kFormats = [] {
        std::vector<EncodedFormat> formats;
        for (const auto& names : Names()) formats.push_back(names.format);
        return formats;
    }();
    return kFormats;
}

const char* ToString(EncodedFormat format) {
    return NamesOf(format).name;
}

std::optional<EncodedFormat> EncodedFormatFromString(const std::string& name) {
    for (const auto& names : Names()) {
        if (name == names.name) return names.format;
    }
    return std::nullopt;
}

std::string IecStream::ToString() const {
    std::ostringstream os;
    os << pcm_channels << "ch " << pcm_rate << "Hz x" << rate_multiplier << (hbr ? " hbr" : "")
       << " aes3=0x" << std::hex << std::setw(2) << std::setfill('0')
       << static_cast<int>(aes3_rate_code);
    return os.str();
}

std::optional<IecStream> IecStreamFor(EncodedFormat format, uint32_t content_rate,
                                      unsigned int channels, bool hbr_allowed) {
    switch (format) {
        case EncodedFormat::kAc3:
        case EncodedFormat::kDts:
            // Bursts at the content rate (IEC 61937-3 / -5 types I..III).
            if (content_rate > 48000) return std::nullopt;
            return TwoChannel(content_rate, 1);
        case EncodedFormat::kEac3:
        case EncodedFormat::kEac3Joc:
            // IEC 61937-3: four times the content rate.
            if (content_rate > 48000) return std::nullopt;
            return TwoChannel(content_rate, 4);
        case EncodedFormat::kIec61937:
            if (channels == kHbrChannels) {
                if (!hbr_allowed || content_rate != kHbrPcmRate) return std::nullopt;
                return IecStream{.pcm_rate = kHbrPcmRate,
                                 .pcm_channels = kHbrChannels,
                                 .hbr = true,
                                 .rate_multiplier = 1,
                                 .aes3_rate_code = Aes3RateCode(kHbrStreamRate)};
            }
            if (channels != 2) return std::nullopt;
            return TwoChannel(content_rate, 1);
    }
    return std::nullopt;
}

std::vector<uint32_t> ContentRates(EncodedFormat format, bool hbr_allowed) {
    std::vector<uint32_t> rates;
    for (const uint32_t rate : kIecRates) {
        if (IecStreamFor(format, rate, 2, hbr_allowed).has_value()) rates.push_back(rate);
    }
    return rates;
}

std::optional<EncodedFormat> FromAidl(const AudioFormatDescription& format) {
    if (format.type != AudioFormatType::NON_PCM) return std::nullopt;
    for (const auto& names : Names()) {
        if (format.encoding != names.mime) continue;
        // IEC 61937 is framed as 16-bit PCM; the other encodings carry no
        // PCM type.
        if (names.format == EncodedFormat::kIec61937 && format.pcm != PcmType::INT_16_BIT) {
            return std::nullopt;
        }
        return names.format;
    }
    return std::nullopt;
}

AudioFormatDescription ToAidl(EncodedFormat format) {
    return AudioFormatDescription{
            .type = AudioFormatType::NON_PCM,
            .pcm = format == EncodedFormat::kIec61937 ? PcmType::INT_16_BIT : PcmType::DEFAULT,
            .encoding = NamesOf(format).mime};
}

audio_format_t ToAudioFormat(EncodedFormat format) {
    return NamesOf(format).audio_format;
}

bool Iec958Status::IsNonAudio() const {
    return (bytes[0] & IEC958_AES0_NONAUDIO) != 0;
}

std::string Iec958Status::ToString() const {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i = 0; i < 4; ++i) {
        os << (i == 0 ? "" : " ") << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return os.str();
}

Iec958Status NonAudioChannelStatus(const IecStream& stream) {
    Iec958Status status;
    status.bytes[0] = IEC958_AES0_CON_NOT_COPYRIGHT | IEC958_AES0_NONAUDIO;
    status.bytes[1] = IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER;
    status.bytes[2] = 0;
    status.bytes[3] = stream.aes3_rate_code;
    return status;
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
