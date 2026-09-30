/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <memory>

#include "passthrough/Encoder.h"
#include "passthrough/Format.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// A packer behind Encoder. Private to Encoder*.cpp / FfmpegEncoder.cpp.
class EncoderBackend {
  public:
    virtual ~EncoderBackend() = default;
    virtual void Write(const void* data, size_t bytes) = 0;
    virtual void Reset() = 0;
};

#ifdef MAINLINE_AUDIO_WITH_FFMPEG
// FFmpeg's "spdif" muxer for kDtsHd, kDtsHdMa and kTrueHd, nullptr on failure.
std::unique_ptr<EncoderBackend> CreateFfmpegBackend(EncodedFormat format, const IecStream& stream,
                                                    Encoder::Output output);
#endif

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
