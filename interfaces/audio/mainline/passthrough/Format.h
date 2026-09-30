/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <aidl/android/media/audio/common/AudioFormatDescription.h>
#include <system/audio.h>

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// Compressed formats the HAL can send to an HDMI sink as IEC 61937 data.
enum class EncodedFormat {
    kAc3,
    kEac3,
    kEac3Joc,  // E-AC-3 with Joint Object Coding (Dolby Atmos in DD+).
    kDts,
    kIec61937,  // Already packed by the application; passed through unchanged.
};

// Every format, in the order profiles list them.
const std::vector<EncodedFormat>& AllEncodedFormats();

// Short name, also used by the hdmi.passthrough_formats property:
// "ac3", "eac3", "eac3-joc", "dts", "iec61937".
const char* ToString(EncodedFormat format);
std::optional<EncodedFormat> EncodedFormatFromString(const std::string& name);

// How a stream is carried over HDMI: the PCM configuration the IEC 61937
// bursts are written with.
struct IecStream {
    uint32_t pcm_rate = 0;
    unsigned int pcm_channels = 2;
    // High bit rate: bursts at 16 times the base rate, sent as 8 channels.
    bool hbr = false;
    // Ratio of the burst rate to the content rate.
    uint32_t rate_multiplier = 1;
    // Sample rate code of IEC 60958-3 channel status byte 3.
    uint8_t aes3_rate_code = 0;

    std::string ToString() const;
};

// The IEC 61937 stream for `format` at `content_rate`. `channels` only
// matters for kIec61937 (2, or 8 for high bit rate, which only applications
// packing the data themselves can use). Returns nullopt when the combination
// has no valid HDMI audio rate or needs high bit rate while `hbr_allowed` is
// false. High bit rate is only offered at 192 kHz: IEC 60958-3 channel status
// has a code for 768 kHz, not for 705.6.
std::optional<IecStream> IecStreamFor(EncodedFormat format, uint32_t content_rate,
                                      unsigned int channels, bool hbr_allowed);

// Content sample rates `format` can be sent at.
std::vector<uint32_t> ContentRates(EncodedFormat format, bool hbr_allowed);

// AIDL <-> EncodedFormat. The only place of the HAL where encoded formats are
// converted.
std::optional<EncodedFormat> FromAidl(
        const ::aidl::android::media::audio::common::AudioFormatDescription& format);
::aidl::android::media::audio::common::AudioFormatDescription ToAidl(EncodedFormat format);

// Format of the libaudiospdif fork, AUDIO_FORMAT_DEFAULT for kIec61937.
audio_format_t ToAudioFormat(EncodedFormat format);

// IEC 60958 channel status as used by ALSA's IEC958 controls.
struct Iec958Status {
    std::array<uint8_t, 24> bytes = {};

    bool IsNonAudio() const;
    std::string ToString() const;
};

// Consumer channel status for IEC 61937 data sent as `stream`: non-audio,
// no copyright, original, PCM coder category, the rate of the burst stream.
Iec958Status NonAudioChannelStatus(const IecStream& stream);

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
