/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Passthrough"

#include "passthrough/Encoder.h"

#include <audio_utils/spdif/SPDIFEncoder.h>

namespace aidl::android::hardware::audio::core::mainline::passthrough {

class Encoder::Impl final : public ::android::SPDIFEncoder {
  public:
    Impl(audio_format_t format, uint32_t rate_multiplier, Output output)
        : SPDIFEncoder(format, rate_multiplier), output_(std::move(output)) {}

    ssize_t writeOutput(const void* buffer, size_t bytes) override {
        output_(static_cast<const uint8_t*>(buffer), bytes);
        return static_cast<ssize_t>(bytes);
    }

  private:
    const Output output_;
};

std::unique_ptr<Encoder> Encoder::Create(EncodedFormat format, uint32_t rate_multiplier,
                                         Output output) {
    const audio_format_t audio_format = ToAudioFormat(format);
    // SPDIFEncoder aborts on a format it does not know.
    if (audio_format == AUDIO_FORMAT_DEFAULT ||
        !::android::SPDIFEncoder::isFormatSupported(audio_format)) {
        return nullptr;
    }
    return std::unique_ptr<Encoder>(
            new Encoder(std::make_unique<Impl>(audio_format, rate_multiplier, std::move(output))));
}

Encoder::Encoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Encoder::~Encoder() = default;

void Encoder::Write(const void* data, size_t bytes) {
    impl_->write(data, bytes);
}

void Encoder::Reset() {
    impl_->reset();
}

unsigned int Encoder::OutputChannels() const {
    return impl_->getOutputChannelCount();
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
