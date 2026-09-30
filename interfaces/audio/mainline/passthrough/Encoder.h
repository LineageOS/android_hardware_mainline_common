/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "passthrough/Format.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

class EncoderBackend;

// Packs an encoded stream into IEC 61937 data bursts. The only user of the
// packers: the libaudiospdif fork in ../spdif for AC-3, E-AC-3 and DTS, and,
// when the HAL is built with the ffmpeg_passthrough option, FFmpeg's "spdif"
// muxer for DTS-HD and Dolby TrueHD. Syncing or replacing either only touches
// Encoder*.cpp / FfmpegEncoder.cpp.
class Encoder {
  public:
    // Receives the bursts, as bytes of 16-bit little endian PCM frames.
    using Output = std::function<void(const uint8_t* data, size_t bytes)>;

    // Whether this build can pack `format`. False for kIec61937, which needs
    // no packing.
    static bool CanPack(EncodedFormat format);

    // nullptr when `format` can not be packed as `stream` (see IecStreamFor()).
    static std::unique_ptr<Encoder> Create(EncodedFormat format, const IecStream& stream,
                                           Output output);
    ~Encoder();

    // Takes encoded data; frames do not have to be aligned. Complete bursts
    // go to the output before this returns.
    void Write(const void* data, size_t bytes);
    // Drops buffered data and waits for the next frame start (flush, seek).
    void Reset();
    // Channels the bursts are sent on.
    unsigned int OutputChannels() const { return output_channels_; }

  private:
    Encoder(std::unique_ptr<EncoderBackend> backend, unsigned int output_channels);

    std::unique_ptr<EncoderBackend> backend_;
    const unsigned int output_channels_;
};

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
