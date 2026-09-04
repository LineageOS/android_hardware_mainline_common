/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <vector>

#include <effect-impl/EffectContext.h>

#include "LegacyLibrary.h"

namespace aidl::android::hardware::audio::effect::legacy {

// EffectContext that owns a legacy effect instance and forwards the common
// parameters to it as EFFECT_CMD_* commands.
class LegacyEffectContext final : public EffectContext {
  public:
    // Takes ownership of `handle`. `is_input` selects the capture flavour of
    // the device command (EFFECT_CMD_SET_INPUT_DEVICE).
    LegacyEffectContext(size_t status_depth, const Parameter::Common& common,
                        std::unique_ptr<LegacyEffectHandle> handle, bool is_input);

    // Sends EFFECT_CMD_INIT and the initial EFFECT_CMD_SET_CONFIG. Must be
    // called once after construction; false means the effect is unusable.
    bool Initialize();

    LegacyEffectHandle& handle() { return *handle_; }
    bool int16_mode() const { return int16_mode_; }

    // EffectContext
    RetCode setCommon(const Parameter::Common& common) override;
    RetCode setOutputDevice(
            const std::vector<::aidl::android::media::audio::common::AudioDeviceDescription>&
                    devices) override;
    RetCode setAudioMode(const ::aidl::android::media::audio::common::AudioMode& mode) override;
    RetCode setAudioSource(
            const ::aidl::android::media::audio::common::AudioSource& source) override;
    RetCode setVolumeStereo(const Parameter::VolumeStereo& volume) override;
    RetCode enable() override;
    RetCode disable() override;
    RetCode reset() override;

    // Runs the legacy process() on `samples` float samples. Handles the
    // 16-bit fallback and differing input / output channel counts.
    IEffect::Status Process(float* in, float* out, int samples);

  private:
    // Applies `common` through EFFECT_CMD_SET_CONFIG, first as float, then
    // as 16-bit PCM when the effect refuses float.
    RetCode ApplyConfig(const Parameter::Common& common);
    int32_t SendConfig(const Parameter::Common& common, audio_format_t format);

    const std::unique_ptr<LegacyEffectHandle> handle_;
    const bool is_input_;
    const bool volume_control_;  // EFFECT_FLAG_VOLUME_CTRL: effect applies volume itself.
    bool int16_mode_ = false;
    std::vector<int16_t> in16_;
    std::vector<int16_t> out16_;
    std::vector<float> out_scratch_;
};

}  // namespace aidl::android::hardware::audio::effect::legacy
