/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Simple"

// Effect types with a few scalar parameters: LoudnessEnhancer, Downmix, Volume.

#include <vector>

#include <aidl/android/hardware/audio/effect/Downmix.h>
#include <aidl/android/hardware/audio/effect/LoudnessEnhancer.h>
#include <aidl/android/hardware/audio/effect/Range.h>
#include <aidl/android/hardware/audio/effect/Volume.h>
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
        // Set takes a downmix_type_t (an int).
        const int32_t type = value.get<Downmix::type>() == Downmix::Type::STRIP ? DOWNMIX_TYPE_STRIP
                                                                                : DOWNMIX_TYPE_FOLD;
        return LegacyStatusToBinder(SetSimple(effect, DOWNMIX_PARAM_TYPE, type),
                                    "DOWNMIX_PARAM_TYPE");
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, Downmix::Tag tag,
                                Downmix* value) override {
        if (tag != Downmix::type) return Unsupported("downmix parameter");
        // The library answers with an int16_t but wants room for an int32_t.
        LegacyParam param(sizeof(uint32_t), sizeof(int32_t));
        param.SetParam<uint32_t>(0, DOWNMIX_PARAM_TYPE);
        if (GetParam(effect, &param) != 0 || param.vsize() < sizeof(int16_t)) {
            return LegacyStatusToBinder(-EINVAL, "DOWNMIX_PARAM_TYPE");
        }
        value->set<Downmix::type>(param.GetValue<int16_t>(0) == DOWNMIX_TYPE_STRIP
                                          ? Downmix::Type::STRIP
                                          : Downmix::Type::FOLD);
        return ndk::ScopedAStatus::ok();
    }
};

// The Volume effect has no parameter header in system/media: its ids are the
// OpenSL ES volume interface ones, as in the AOSP bundle (EffectBundle.h).
constexpr uint32_t kVolumeParamLevel = 0;  // int16_t, millibel
constexpr uint32_t kVolumeParamMute = 2;   // uint32_t, boolean

constexpr int32_t kMinVolumeDb = -96;
constexpr int32_t kMaxVolumeDb = 0;

class VolumeTranslator final
    : public TypedTranslator<Volume, Parameter::Specific::volume, Parameter::Id::volumeTag> {
  public:
    void FillCapability(LegacyEffectHandle& /*effect*/, Capability* capability) override {
        Range::VolumeRange range;
        range.min.set<Volume::levelDb>(kMinVolumeDb);
        range.max.set<Volume::levelDb>(kMaxVolumeDb);
        capability->range.set<Range::volume>(std::vector<Range::VolumeRange>{range});
    }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const Volume& value) override {
        switch (value.getTag()) {
            case Volume::levelDb: {
                const int32_t level_db = value.get<Volume::levelDb>();
                if (level_db < kMinVolumeDb || level_db > kMaxVolumeDb) {
                    return IllegalArgument("volume level out of range");
                }
                return LegacyStatusToBinder(
                        SetSimple(effect, kVolumeParamLevel, static_cast<int16_t>(level_db * 100)),
                        "VOLUME_PARAM_LEVEL");
            }
            case Volume::mute:
                return LegacyStatusToBinder(
                        SetSimple(effect, kVolumeParamMute,
                                  static_cast<uint32_t>(value.get<Volume::mute>() ? 1 : 0)),
                        "VOLUME_PARAM_MUTE");
            default:
                return Unsupported("volume parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, Volume::Tag tag,
                                Volume* value) override {
        switch (tag) {
            case Volume::levelDb: {
                const auto level = GetSimple<int16_t>(effect, kVolumeParamLevel);
                if (!level.has_value()) return LegacyStatusToBinder(-EINVAL, "VOLUME_PARAM_LEVEL");
                value->set<Volume::levelDb>(*level / 100);
                return ndk::ScopedAStatus::ok();
            }
            case Volume::mute: {
                const auto mute = GetSimple<uint32_t>(effect, kVolumeParamMute);
                if (!mute.has_value()) return LegacyStatusToBinder(-EINVAL, "VOLUME_PARAM_MUTE");
                value->set<Volume::mute>(*mute != 0);
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("volume parameter");
        }
    }
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateVolumeTranslator() {
    return std::make_unique<VolumeTranslator>();
}

std::unique_ptr<ParameterTranslator> CreateLoudnessEnhancerTranslator() {
    return std::make_unique<LoudnessEnhancerTranslator>();
}

std::unique_ptr<ParameterTranslator> CreateDownmixTranslator() {
    return std::make_unique<DownmixTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
