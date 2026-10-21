/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Haptic"

// HapticGenerator: haptic scales and the vibrator information. The legacy
// effect takes both with SET_PARAM only, so the translator keeps what it was
// given to answer a get.

#include <map>
#include <vector>

#include <aidl/android/hardware/audio/effect/HapticGenerator.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_hapticgenerator.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

class HapticGeneratorTranslator final
    : public TypedTranslator<HapticGenerator, Parameter::Specific::hapticGenerator,
                             Parameter::Id::hapticGeneratorTag> {
    void OnOpen(LegacyEffectHandle& /*effect*/) override {
        scales_.clear();
        vibrator_info_ = {};
    }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const HapticGenerator& value) override {
        switch (value.getTag()) {
            case HapticGenerator::hapticScales: {
                // One HG_PARAM_HAPTIC_INTENSITY per scale: id, scale, then the two factors.
                const auto& scales = value.get<HapticGenerator::hapticScales>();
                for (const auto& scale : scales) {
                    LegacyParam param(sizeof(uint32_t), 2 * sizeof(int32_t) + 2 * sizeof(float));
                    param.SetParam<uint32_t>(0, HG_PARAM_HAPTIC_INTENSITY);
                    param.SetValue<int32_t>(0, scale.id);
                    param.SetValue<int32_t>(sizeof(int32_t), static_cast<int32_t>(scale.scale));
                    param.SetValue<float>(2 * sizeof(int32_t), scale.scaleFactor);
                    param.SetValue<float>(2 * sizeof(int32_t) + sizeof(float),
                                          scale.adaptiveScaleFactor);
                    if (auto status = LegacyStatusToBinder(SetParam(effect, param),
                                                           "HG_PARAM_HAPTIC_INTENSITY");
                        !status.isOk()) {
                        return status;
                    }
                    scales_[scale.id] = scale;
                }
                return ndk::ScopedAStatus::ok();
            }
            case HapticGenerator::vibratorInfo: {
                const auto& info = value.get<HapticGenerator::vibratorInfo>();
                LegacyParam param(sizeof(uint32_t), 3 * sizeof(float));
                param.SetParam<uint32_t>(0, HG_PARAM_VIBRATOR_INFO);
                param.SetValue<float>(0, info.resonantFrequencyHz);
                param.SetValue<float>(sizeof(float), info.qFactor);
                param.SetValue<float>(2 * sizeof(float), info.maxAmplitude);
                if (auto status =
                            LegacyStatusToBinder(SetParam(effect, param), "HG_PARAM_VIBRATOR_INFO");
                    !status.isOk()) {
                    return status;
                }
                vibrator_info_ = info;
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("haptic generator parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& /*effect*/, HapticGenerator::Tag tag,
                                HapticGenerator* value) override {
        switch (tag) {
            case HapticGenerator::hapticScales: {
                std::vector<HapticGenerator::HapticScale> scales;
                for (const auto& [id, scale] : scales_) scales.push_back(scale);
                value->set<HapticGenerator::hapticScales>(scales);
                return ndk::ScopedAStatus::ok();
            }
            case HapticGenerator::vibratorInfo:
                value->set<HapticGenerator::vibratorInfo>(vibrator_info_);
                return ndk::ScopedAStatus::ok();
            default:
                return Unsupported("haptic generator parameter");
        }
    }

  private:
    std::map<int32_t, HapticGenerator::HapticScale> scales_;
    HapticGenerator::VibratorInformation vibrator_info_;
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateHapticGeneratorTranslator() {
    return std::make_unique<HapticGeneratorTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
