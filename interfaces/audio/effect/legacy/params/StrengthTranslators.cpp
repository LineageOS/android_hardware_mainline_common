/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Strength"

// BassBoost and Virtualizer: both are "strength in per-mille" effects.

#include <vector>

#include <aidl/android/hardware/audio/effect/BassBoost.h>
#include <aidl/android/hardware/audio/effect/Range.h>
#include <aidl/android/hardware/audio/effect/Virtualizer.h>
#include <android-base/logging.h>
#include <media/AidlConversionCppNdk.h>
#include <system/audio.h>
#include <system/audio_effects/effect_bassboost.h>
#include <system/audio_effects/effect_virtualizer.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

using ::aidl::android::media::audio::common::AudioDeviceDescription;

namespace {

constexpr int32_t kMinStrength = 0;
constexpr int32_t kMaxStrength = 1000;

// Reads the "*_PARAM_STRENGTH_SUPPORTED" boolean of either effect type.
bool StrengthSupported(LegacyEffectHandle& effect, uint32_t supported_id) {
    const auto supported = GetSimple<uint32_t>(effect, supported_id);
    return supported.has_value() && *supported != 0;
}

class BassBoostTranslator final : public TypedTranslator<BassBoost, Parameter::Specific::bassBoost,
                                                         Parameter::Id::bassBoostTag> {
  public:
    void FillCapability(LegacyEffectHandle& effect, Capability* capability) override {
        // The framework answers BASSBOOST_PARAM_STRENGTH_SUPPORTED from the
        // presence of a valid strength range in the capability.
        if (!StrengthSupported(effect, BASSBOOST_PARAM_STRENGTH_SUPPORTED)) return;
        Range::BassBoostRange range;
        range.min.set<BassBoost::strengthPm>(kMinStrength);
        range.max.set<BassBoost::strengthPm>(kMaxStrength);
        capability->range.set<Range::bassBoost>(std::vector<Range::BassBoostRange>{range});
    }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const BassBoost& value) override {
        if (value.getTag() != BassBoost::strengthPm) return Unsupported("bass boost parameter");
        const auto strength = static_cast<int16_t>(value.get<BassBoost::strengthPm>());
        return LegacyStatusToBinder(SetSimple(effect, BASSBOOST_PARAM_STRENGTH, strength),
                                    "BASSBOOST_PARAM_STRENGTH");
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, BassBoost::Tag tag,
                                BassBoost* value) override {
        if (tag != BassBoost::strengthPm) return Unsupported("bass boost parameter");
        const auto strength = GetSimple<int16_t>(effect, BASSBOOST_PARAM_STRENGTH);
        if (!strength.has_value()) return LegacyStatusToBinder(-EINVAL, "BASSBOOST_PARAM_STRENGTH");
        value->set<BassBoost::strengthPm>(*strength);
        return ndk::ScopedAStatus::ok();
    }
};

class VirtualizerTranslator final
    : public TypedTranslator<Virtualizer, Parameter::Specific::virtualizer,
                             Parameter::Id::virtualizerTag> {
  public:
    void FillCapability(LegacyEffectHandle& effect, Capability* capability) override {
        if (!StrengthSupported(effect, VIRTUALIZER_PARAM_STRENGTH_SUPPORTED)) return;
        Range::VirtualizerRange range;
        range.min.set<Virtualizer::strengthPm>(kMinStrength);
        range.max.set<Virtualizer::strengthPm>(kMaxStrength);
        capability->range.set<Range::virtualizer>(std::vector<Range::VirtualizerRange>{range});
    }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const Virtualizer& value) override {
        switch (value.getTag()) {
            case Virtualizer::strengthPm: {
                const auto strength = static_cast<int16_t>(value.get<Virtualizer::strengthPm>());
                return LegacyStatusToBinder(SetSimple(effect, VIRTUALIZER_PARAM_STRENGTH, strength),
                                            "VIRTUALIZER_PARAM_STRENGTH");
            }
            case Virtualizer::device: {
                // Forced virtualization mode.
                const auto device =
                        ::aidl::android::aidl2legacy_AudioDeviceDescription_audio_devices_t(
                                value.get<Virtualizer::device>());
                if (!device.ok()) return IllegalArgument("virtualizer device");
                return LegacyStatusToBinder(
                        SetSimple(effect, VIRTUALIZER_PARAM_FORCE_VIRTUALIZATION_MODE,
                                  static_cast<int32_t>(device.value())),
                        "VIRTUALIZER_PARAM_FORCE_VIRTUALIZATION_MODE");
            }
            default:
                return Unsupported("virtualizer parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, Virtualizer::Tag tag,
                                Virtualizer* value) override {
        switch (tag) {
            case Virtualizer::strengthPm: {
                const auto strength = GetSimple<int16_t>(effect, VIRTUALIZER_PARAM_STRENGTH);
                if (!strength.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "VIRTUALIZER_PARAM_STRENGTH");
                }
                value->set<Virtualizer::strengthPm>(*strength);
                return ndk::ScopedAStatus::ok();
            }
            case Virtualizer::device: {
                const auto device =
                        GetSimple<int32_t>(effect, VIRTUALIZER_PARAM_VIRTUALIZATION_MODE);
                if (!device.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "VIRTUALIZER_PARAM_VIRTUALIZATION_MODE");
                }
                const auto aidl_device =
                        ::aidl::android::legacy2aidl_audio_devices_t_AudioDeviceDescription(
                                static_cast<audio_devices_t>(*device));
                if (!aidl_device.ok()) return IllegalArgument("virtualization mode device");
                value->set<Virtualizer::device>(aidl_device.value());
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("virtualizer parameter");
        }
    }

    // Virtualizer::Id::speakerAnglesPayload -> VIRTUALIZER_PARAM_VIRTUAL_SPEAKER_ANGLES.
    ndk::ScopedAStatus GetExtra(LegacyEffectHandle& effect, const Virtualizer::Id& id,
                                Virtualizer* value) override {
        if (id.getTag() != Virtualizer::Id::speakerAnglesPayload) {
            return Unsupported("virtualizer id variant");
        }
        const auto& payload = id.get<Virtualizer::Id::speakerAnglesPayload>();
        const auto mask = ::aidl::android::aidl2legacy_AudioChannelLayout_audio_channel_mask_t(
                payload.layout, false /*isInput*/);
        const auto device =
                ::aidl::android::aidl2legacy_AudioDeviceDescription_audio_devices_t(payload.device);
        if (!mask.ok() || !device.ok()) return IllegalArgument("speaker angles payload");

        const unsigned int channels = audio_channel_count_from_out_mask(mask.value());
        // One (channel mask, azimuth, elevation) int32 triplet per channel.
        LegacyParam param(3 * sizeof(uint32_t), channels * 3 * sizeof(int32_t));
        param.SetParam<uint32_t>(0, VIRTUALIZER_PARAM_VIRTUAL_SPEAKER_ANGLES);
        param.SetParam<uint32_t>(1, static_cast<uint32_t>(mask.value()));
        param.SetParam<uint32_t>(2, static_cast<uint32_t>(device.value()));
        auto status = LegacyStatusToBinder(GetParam(effect, &param),
                                           "VIRTUALIZER_PARAM_VIRTUAL_SPEAKER_ANGLES");
        if (!status.isOk()) return status;

        std::vector<Virtualizer::ChannelAngle> angles;
        for (size_t offset = 0; offset + 3 * sizeof(int32_t) <= param.vsize();
             offset += 3 * sizeof(int32_t)) {
            const auto channel_mask =
                    static_cast<audio_channel_mask_t>(param.GetValue<uint32_t>(offset));
            const int channel = ::aidl::android::
                    legacy2aidl_audio_channel_mask_t_bits_AudioChannelLayout_layout(
                            channel_mask, false /*isInput*/);
            angles.push_back(
                    {.channel = channel,
                     .azimuthDegree = param.GetValue<int32_t>(offset + sizeof(int32_t)),
                     .elevationDegree = param.GetValue<int32_t>(offset + 2 * sizeof(int32_t))});
        }
        value->set<Virtualizer::speakerAngles>(angles);
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateBassBoostTranslator() {
    return std::make_unique<BassBoostTranslator>();
}

std::unique_ptr<ParameterTranslator> CreateVirtualizerTranslator() {
    return std::make_unique<VirtualizerTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
