/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Passthrough"

#include "passthrough/Encoder.h"

#include <audio_utils/spdif/SPDIFEncoder.h>

#include "passthrough/EncoderBackend.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

namespace {

// The libaudiospdif fork.
class SpdifBackend final : public EncoderBackend, private ::android::SPDIFEncoder {
  public:
    SpdifBackend(audio_format_t format, Encoder::Output output)
        : SPDIFEncoder(format), output_(std::move(output)) {}

    void Write(const void* data, size_t bytes) override { write(data, bytes); }
    void Reset() override { reset(); }

  private:
    ssize_t writeOutput(const void* buffer, size_t bytes) override {
        output_(static_cast<const uint8_t*>(buffer), bytes);
        return static_cast<ssize_t>(bytes);
    }

    const Encoder::Output output_;
};

bool NeedsFfmpeg(EncodedFormat format) {
    return format == EncodedFormat::kDtsHd || format == EncodedFormat::kDtsHdMa ||
           format == EncodedFormat::kTrueHd;
}

}  // namespace

bool Encoder::CanPack(EncodedFormat format) {
    if (NeedsFfmpeg(format)) {
#ifdef MAINLINE_AUDIO_WITH_FFMPEG
        return true;
#else
        return false;
#endif
    }
    const audio_format_t audio_format = ToAudioFormat(format);
    return audio_format != AUDIO_FORMAT_DEFAULT &&
           ::android::SPDIFEncoder::isFormatSupported(audio_format);
}

std::unique_ptr<Encoder> Encoder::Create(EncodedFormat format,
                                         [[maybe_unused]] const IecStream& stream, Output output) {
    if (!CanPack(format)) return nullptr;
    if (NeedsFfmpeg(format)) {
#ifdef MAINLINE_AUDIO_WITH_FFMPEG
        auto backend = CreateFfmpegBackend(format, stream, std::move(output));
        if (backend == nullptr) return nullptr;
        // The muxer writes the same byte stream whatever the channel count.
        return std::unique_ptr<Encoder>(new Encoder(std::move(backend), stream.pcm_channels));
#else
        return nullptr;
#endif
    }
    // The fork sends every burst as two channel PCM (and aborts on a format
    // it does not know, hence CanPack() first).
    return std::unique_ptr<Encoder>(
            new Encoder(std::make_unique<SpdifBackend>(ToAudioFormat(format), std::move(output)),
                        ::android::kSpdifEncodedChannelCount));
}

Encoder::Encoder(std::unique_ptr<EncoderBackend> backend, unsigned int output_channels)
    : backend_(std::move(backend)), output_channels_(output_channels) {}

Encoder::~Encoder() = default;

void Encoder::Write(const void* data, size_t bytes) {
    backend_->Write(data, bytes);
}

void Encoder::Reset() {
    backend_->Reset();
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
