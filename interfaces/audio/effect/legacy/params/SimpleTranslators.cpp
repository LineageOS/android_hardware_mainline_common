/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Simple"

// Effect types with a single scalar parameter: LoudnessEnhancer, Downmix.

#include <aidl/android/hardware/audio/effect/Downmix.h>
#include <aidl/android/hardware/audio/effect/LoudnessEnhancer.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_downmix.h>
#include <system/audio_effects/effect_loudnessenhancer.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

class LoudnessEnhancerTranslator final
    : public TypedTranslator<LoudnessEnhancer, Parameter::Specific::loudnessEnhancer,
                             Parameter::Id::loudnessEnhancerTag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect,
                                const LoudnessEnhancer& value) override {
        if (value.getTag() != LoudnessEnhancer::gainMb) {
            return Unsupported("loudness enhancer parameter");
        }
        return LegacyStatusToBinder(
                SetSimple(effect, LOUDNESS_ENHANCER_PARAM_TARGET_GAIN_MB,
                          static_cast<int32_t>(value.get<LoudnessEnhancer::gainMb>())),
                "LOUDNESS_ENHANCER_PARAM_TARGET_GAIN_MB");
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, LoudnessEnhancer::Tag tag,
                                LoudnessEnhancer* value) override {
        if (tag != LoudnessEnhancer::gainMb) return Unsupported("loudness enhancer parameter");
        const auto gain = GetSimple<int32_t>(effect, LOUDNESS_ENHANCER_PARAM_TARGET_GAIN_MB);
        if (!gain.has_value()) {
            return LegacyStatusToBinder(-EINVAL, "LOUDNESS_ENHANCER_PARAM_TARGET_GAIN_MB");
        }
        value->set<LoudnessEnhancer::gainMb>(*gain);
        return ndk::ScopedAStatus::ok();
    }
};

class DownmixTranslator final
    : public TypedTranslator<Downmix, Parameter::Specific::downmix, Parameter::Id::downmixTag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const Downmix& value) override {
        if (value.getTag() != Downmix::type) return Unsupported("downmix parameter");
        const int16_t type = value.get<Downmix::type>() == Downmix::Type::STRIP ? DOWNMIX_TYPE_STRIP
                                                                                : DOWNMIX_TYPE_FOLD;
        return LegacyStatusToBinder(SetSimple(effect, DOWNMIX_PARAM_TYPE, type),
                                    "DOWNMIX_PARAM_TYPE");
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, Downmix::Tag tag,
                                Downmix* value) override {
        if (tag != Downmix::type) return Unsupported("downmix parameter");
        const auto type = GetSimple<int16_t>(effect, DOWNMIX_PARAM_TYPE);
        if (!type.has_value()) return LegacyStatusToBinder(-EINVAL, "DOWNMIX_PARAM_TYPE");
        value->set<Downmix::type>(*type == DOWNMIX_TYPE_STRIP ? Downmix::Type::STRIP
                                                              : Downmix::Type::FOLD);
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateLoudnessEnhancerTranslator() {
    return std::make_unique<LoudnessEnhancerTranslator>();
}

std::unique_ptr<ParameterTranslator> CreateDownmixTranslator() {
    return std::make_unique<DownmixTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
