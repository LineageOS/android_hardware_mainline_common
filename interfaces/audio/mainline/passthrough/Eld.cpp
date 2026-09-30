/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Passthrough"

#include "passthrough/Eld.h"

#include <algorithm>
#include <string>

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core::mainline::passthrough {

namespace {

// ELD header (4 bytes) and baseline block up to the monitor name.
constexpr size_t kFixedBytes = 20;
constexpr size_t kMaxMonitorNameLength = 16;
constexpr size_t kSadBytes = 3;
// Byte 0 bits 7..3.
constexpr unsigned int kEldVersionCea861D = 2;
constexpr unsigned int kEldVersionPartial = 31;

// CTA-861 audio format codes of a short audio descriptor.
enum AudioFormatCode : unsigned int {
    kLpcm = 1,
    kAc3 = 2,
    kDts = 7,
    kEac3 = 10,
    kDtsHd = 11,
    kMlp = 12,  // Dolby TrueHD
};

// Short audio descriptor byte 1, bits 0..6.
constexpr uint32_t kSadRates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000};

void Add(SinkCapabilities* caps, EncodedFormat format, const std::set<uint32_t>& rates,
         unsigned int channels) {
    SinkCapabilities::Format& entry = caps->formats[format];
    entry.rates.insert(rates.begin(), rates.end());
    entry.max_channels = std::max(entry.max_channels, channels);
}

}  // namespace

std::optional<SinkCapabilities> ParseEld(const std::vector<uint8_t>& eld) {
    if (eld.size() < kFixedBytes ||
        std::all_of(eld.begin(), eld.end(), [](uint8_t b) { return b == 0; })) {
        return std::nullopt;
    }
    const unsigned int version = eld[0] >> 3;
    if (version != kEldVersionCea861D && version != kEldVersionPartial) {
        LOG(WARNING) << __func__ << ": unknown ELD version " << version;
        return std::nullopt;
    }
    const size_t name_length = eld[4] & 0x1f;
    const size_t sad_count = eld[5] >> 4;
    if (name_length > kMaxMonitorNameLength ||
        kFixedBytes + name_length + sad_count * kSadBytes > eld.size()) {
        LOG(WARNING) << __func__ << ": malformed ELD: " << eld.size() << " bytes, name length "
                     << name_length << ", " << sad_count << " SAD(s)";
        return std::nullopt;
    }

    SinkCapabilities caps;
    caps.source = SinkCapabilities::Source::kEld;
    caps.monitor_name.assign(eld.begin() + kFixedBytes, eld.begin() + kFixedBytes + name_length);
    caps.monitor_name.erase(caps.monitor_name.find_last_not_of('\0') + 1);

    for (size_t i = 0; i < sad_count; ++i) {
        const uint8_t* sad = eld.data() + kFixedBytes + name_length + i * kSadBytes;
        const unsigned int code = (sad[0] >> 3) & 0x0f;
        const unsigned int channels = (sad[0] & 0x07) + 1;
        std::set<uint32_t> rates;
        for (size_t bit = 0; bit < std::size(kSadRates); ++bit) {
            if ((sad[1] & (1u << bit)) != 0) rates.insert(kSadRates[bit]);
        }
        switch (code) {
            case kLpcm:
                caps.max_pcm_channels = std::max(caps.max_pcm_channels, channels);
                break;
            case kAc3:
                Add(&caps, EncodedFormat::kAc3, rates, channels);
                break;
            case kDts:
                Add(&caps, EncodedFormat::kDts, rates, channels);
                break;
            case kEac3:
                Add(&caps, EncodedFormat::kEac3, rates, channels);
                // Byte 2 bit 0: Joint Object Coding (Dolby Atmos) supported.
                if ((sad[2] & 0x01) != 0) Add(&caps, EncodedFormat::kEac3Joc, rates, channels);
                break;
            case kDtsHd:
                // The descriptor does not tell High Resolution Audio from
                // Master Audio.
                Add(&caps, EncodedFormat::kDtsHd, rates, channels);
                Add(&caps, EncodedFormat::kDtsHdMa, rates, channels);
                break;
            case kMlp:
                // Byte 2 bit 0 announces Dolby Atmos in MAT, which needs no
                // separate format: Atmos travels inside the TrueHD stream.
                Add(&caps, EncodedFormat::kTrueHd, rates, channels);
                break;
            default:
                break;
        }
    }
    return caps;
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
