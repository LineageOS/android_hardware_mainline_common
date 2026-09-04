/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_PreProc"

// Capture side effects: AEC, NS, AGC (v1) and AGC2.

#include <aidl/android/hardware/audio/effect/AcousticEchoCanceler.h>
#include <aidl/android/hardware/audio/effect/AutomaticGainControlV1.h>
#include <aidl/android/hardware/audio/effect/AutomaticGainControlV2.h>
#include <aidl/android/hardware/audio/effect/NoiseSuppression.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_aec.h>
#include <system/audio_effects/effect_agc.h>
#include <system/audio_effects/effect_agc2.h>
#include <system/audio_effects/effect_ns.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

class AecTranslator final
    : public TypedTranslator<AcousticEchoCanceler, Parameter::Specific::acousticEchoCanceler,
                             Parameter::Id::acousticEchoCancelerTag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect,
                                const AcousticEchoCanceler& value) override {
        switch (value.getTag()) {
            case AcousticEchoCanceler::echoDelayUs:
                return LegacyStatusToBinder(
                        SetSimple(effect, AEC_PARAM_ECHO_DELAY,
                                  static_cast<uint32_t>(
                                          value.get<AcousticEchoCanceler::echoDelayUs>())),
                        "AEC_PARAM_ECHO_DELAY");
            case AcousticEchoCanceler::mobileMode:
                return LegacyStatusToBinder(
                        SetSimple(effect, AEC_PARAM_MOBILE_MODE,
                                  static_cast<uint32_t>(
                                          value.get<AcousticEchoCanceler::mobileMode>() ? 1 : 0)),
                        "AEC_PARAM_MOBILE_MODE");
            default:
                return Unsupported("AEC parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, AcousticEchoCanceler::Tag tag,
                                AcousticEchoCanceler* value) override {
        switch (tag) {
            case AcousticEchoCanceler::echoDelayUs: {
                const auto delay = GetSimple<uint32_t>(effect, AEC_PARAM_ECHO_DELAY);
                if (!delay.has_value())
                    return LegacyStatusToBinder(-EINVAL, "AEC_PARAM_ECHO_DELAY");
                value->set<AcousticEchoCanceler::echoDelayUs>(static_cast<int32_t>(*delay));
                return ndk::ScopedAStatus::ok();
            }
            case AcousticEchoCanceler::mobileMode: {
                const auto mode = GetSimple<uint32_t>(effect, AEC_PARAM_MOBILE_MODE);
                if (!mode.has_value())
                    return LegacyStatusToBinder(-EINVAL, "AEC_PARAM_MOBILE_MODE");
                value->set<AcousticEchoCanceler::mobileMode>(*mode != 0);
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("AEC parameter");
        }
    }
};

class NoiseSuppressionTranslator final
    : public TypedTranslator<NoiseSuppression, Parameter::Specific::noiseSuppression,
                             Parameter::Id::noiseSuppressionTag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect,
                                const NoiseSuppression& value) override {
        switch (value.getTag()) {
            case NoiseSuppression::level:
                return LegacyStatusToBinder(
                        SetSimple(effect, NS_PARAM_LEVEL,
                                  static_cast<int32_t>(value.get<NoiseSuppression::level>())),
                        "NS_PARAM_LEVEL");
            case NoiseSuppression::type:
                return LegacyStatusToBinder(
                        SetSimple(effect, NS_PARAM_TYPE,
                                  static_cast<int32_t>(value.get<NoiseSuppression::type>())),
                        "NS_PARAM_TYPE");
            default:
                return Unsupported("NS parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, NoiseSuppression::Tag tag,
                                NoiseSuppression* value) override {
        switch (tag) {
            case NoiseSuppression::level: {
                const auto level = GetSimple<int32_t>(effect, NS_PARAM_LEVEL);
                if (!level.has_value()) return LegacyStatusToBinder(-EINVAL, "NS_PARAM_LEVEL");
                value->set<NoiseSuppression::level>(static_cast<NoiseSuppression::Level>(*level));
                return ndk::ScopedAStatus::ok();
            }
            case NoiseSuppression::type: {
                const auto type = GetSimple<int32_t>(effect, NS_PARAM_TYPE);
                if (!type.has_value()) return LegacyStatusToBinder(-EINVAL, "NS_PARAM_TYPE");
                value->set<NoiseSuppression::type>(static_cast<NoiseSuppression::Type>(*type));
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("NS parameter");
        }
    }
};

class Agc1Translator final
    : public TypedTranslator<AutomaticGainControlV1, Parameter::Specific::automaticGainControlV1,
                             Parameter::Id::automaticGainControlV1Tag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect,
                                const AutomaticGainControlV1& value) override {
        switch (value.getTag()) {
            case AutomaticGainControlV1::targetPeakLevelDbFs:
                return LegacyStatusToBinder(
                        SetSimple(
                                effect, AGC_PARAM_TARGET_LEVEL,
                                static_cast<int16_t>(
                                        value.get<AutomaticGainControlV1::targetPeakLevelDbFs>())),
                        "AGC_PARAM_TARGET_LEVEL");
            case AutomaticGainControlV1::maxCompressionGainDb:
                return LegacyStatusToBinder(
                        SetSimple(
                                effect, AGC_PARAM_COMP_GAIN,
                                static_cast<int16_t>(
                                        value.get<AutomaticGainControlV1::maxCompressionGainDb>())),
                        "AGC_PARAM_COMP_GAIN");
            case AutomaticGainControlV1::enableLimiter:
                return LegacyStatusToBinder(
                        SetSimple(effect, AGC_PARAM_LIMITER_ENA,
                                  value.get<AutomaticGainControlV1::enableLimiter>()),
                        "AGC_PARAM_LIMITER_ENA");
            default:
                return Unsupported("AGC parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, AutomaticGainControlV1::Tag tag,
                                AutomaticGainControlV1* value) override {
        switch (tag) {
            case AutomaticGainControlV1::targetPeakLevelDbFs: {
                const auto level = GetSimple<int16_t>(effect, AGC_PARAM_TARGET_LEVEL);
                if (!level.has_value())
                    return LegacyStatusToBinder(-EINVAL, "AGC_PARAM_TARGET_LEVEL");
                value->set<AutomaticGainControlV1::targetPeakLevelDbFs>(*level);
                return ndk::ScopedAStatus::ok();
            }
            case AutomaticGainControlV1::maxCompressionGainDb: {
                const auto gain = GetSimple<int16_t>(effect, AGC_PARAM_COMP_GAIN);
                if (!gain.has_value()) return LegacyStatusToBinder(-EINVAL, "AGC_PARAM_COMP_GAIN");
                value->set<AutomaticGainControlV1::maxCompressionGainDb>(*gain);
                return ndk::ScopedAStatus::ok();
            }
            case AutomaticGainControlV1::enableLimiter: {
                const auto enable = GetSimple<bool>(effect, AGC_PARAM_LIMITER_ENA);
                if (!enable.has_value())
                    return LegacyStatusToBinder(-EINVAL, "AGC_PARAM_LIMITER_ENA");
                value->set<AutomaticGainControlV1::enableLimiter>(*enable);
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("AGC parameter");
        }
    }
};

class Agc2Translator final
    : public TypedTranslator<AutomaticGainControlV2, Parameter::Specific::automaticGainControlV2,
                             Parameter::Id::automaticGainControlV2Tag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect,
                                const AutomaticGainControlV2& value) override {
        switch (value.getTag()) {
            case AutomaticGainControlV2::fixedDigitalGainMb:
                return LegacyStatusToBinder(
                        SetSimple(effect, AGC2_PARAM_FIXED_DIGITAL_GAIN,
                                  static_cast<uint32_t>(
                                          value.get<AutomaticGainControlV2::fixedDigitalGainMb>())),
                        "AGC2_PARAM_FIXED_DIGITAL_GAIN");
            case AutomaticGainControlV2::levelEstimator:
                return LegacyStatusToBinder(
                        SetSimple(effect, AGC2_PARAM_ADAPT_DIGI_LEVEL_ESTIMATOR,
                                  static_cast<uint32_t>(
                                          value.get<AutomaticGainControlV2::levelEstimator>())),
                        "AGC2_PARAM_ADAPT_DIGI_LEVEL_ESTIMATOR");
            case AutomaticGainControlV2::saturationMarginMb:
                return LegacyStatusToBinder(
                        SetSimple(effect, AGC2_PARAM_ADAPT_DIGI_EXTRA_SATURATION_MARGIN,
                                  static_cast<uint32_t>(
                                          value.get<AutomaticGainControlV2::saturationMarginMb>())),
                        "AGC2_PARAM_ADAPT_DIGI_EXTRA_SATURATION_MARGIN");
            default:
                return Unsupported("AGC2 parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, AutomaticGainControlV2::Tag tag,
                                AutomaticGainControlV2* value) override {
        switch (tag) {
            case AutomaticGainControlV2::fixedDigitalGainMb: {
                const auto gain = GetSimple<uint32_t>(effect, AGC2_PARAM_FIXED_DIGITAL_GAIN);
                if (!gain.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "AGC2_PARAM_FIXED_DIGITAL_GAIN");
                }
                value->set<AutomaticGainControlV2::fixedDigitalGainMb>(static_cast<int32_t>(*gain));
                return ndk::ScopedAStatus::ok();
            }
            case AutomaticGainControlV2::levelEstimator: {
                const auto estimator =
                        GetSimple<uint32_t>(effect, AGC2_PARAM_ADAPT_DIGI_LEVEL_ESTIMATOR);
                if (!estimator.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "AGC2_PARAM_ADAPT_DIGI_LEVEL_ESTIMATOR");
                }
                value->set<AutomaticGainControlV2::levelEstimator>(
                        static_cast<AutomaticGainControlV2::LevelEstimator>(*estimator));
                return ndk::ScopedAStatus::ok();
            }
            case AutomaticGainControlV2::saturationMarginMb: {
                const auto margin =
                        GetSimple<uint32_t>(effect, AGC2_PARAM_ADAPT_DIGI_EXTRA_SATURATION_MARGIN);
                if (!margin.has_value()) {
                    return LegacyStatusToBinder(-EINVAL,
                                                "AGC2_PARAM_ADAPT_DIGI_EXTRA_SATURATION_MARGIN");
                }
                value->set<AutomaticGainControlV2::saturationMarginMb>(
                        static_cast<int32_t>(*margin));
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("AGC2 parameter");
        }
    }
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateAecTranslator() {
    return std::make_unique<AecTranslator>();
}

std::unique_ptr<ParameterTranslator> CreateNoiseSuppressionTranslator() {
    return std::make_unique<NoiseSuppressionTranslator>();
}

std::unique_ptr<ParameterTranslator> CreateAgc1Translator() {
    return std::make_unique<Agc1Translator>();
}

std::unique_ptr<ParameterTranslator> CreateAgc2Translator() {
    return std::make_unique<Agc2Translator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
