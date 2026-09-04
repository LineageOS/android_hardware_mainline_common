/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Reverb"

#include <vector>

#include <aidl/android/hardware/audio/effect/EnvironmentalReverb.h>
#include <aidl/android/hardware/audio/effect/PresetReverb.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_environmentalreverb.h>
#include <system/audio_effects/effect_presetreverb.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

// --- Preset reverb -----------------------------------------------------------

class PresetReverbTranslator final
    : public TypedTranslator<PresetReverb, Parameter::Specific::presetReverb,
                             Parameter::Id::presetReverbTag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const PresetReverb& value) override {
        if (value.getTag() != PresetReverb::preset) return Unsupported("preset reverb parameter");
        const auto preset = static_cast<uint16_t>(value.get<PresetReverb::preset>());
        return LegacyStatusToBinder(SetSimple(effect, REVERB_PARAM_PRESET, preset),
                                    "REVERB_PARAM_PRESET");
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, PresetReverb::Tag tag,
                                PresetReverb* value) override {
        switch (tag) {
            case PresetReverb::preset: {
                const auto preset = GetSimple<uint16_t>(effect, REVERB_PARAM_PRESET);
                if (!preset.has_value())
                    return LegacyStatusToBinder(-EINVAL, "REVERB_PARAM_PRESET");
                value->set<PresetReverb::preset>(static_cast<PresetReverb::Presets>(*preset));
                return ndk::ScopedAStatus::ok();
            }
            case PresetReverb::supportedPresets: {
                // The legacy API has no query for this; every OpenSL ES preset
                // is supported by definition.
                std::vector<PresetReverb::Presets> presets;
                for (int p = REVERB_PRESET_NONE; p <= REVERB_PRESET_LAST; ++p) {
                    presets.push_back(static_cast<PresetReverb::Presets>(p));
                }
                value->set<PresetReverb::supportedPresets>(presets);
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("preset reverb parameter");
        }
    }
};

// --- Environmental reverb ----------------------------------------------------

// Legacy value type per parameter id.
enum class ValueType { kInt16, kUint32, kBool };

struct EnvReverbField {
    EnvironmentalReverb::Tag tag;
    uint32_t legacy_id;
    ValueType type;
};

constexpr EnvReverbField kEnvReverbFields[] = {
        {EnvironmentalReverb::roomLevelMb, REVERB_PARAM_ROOM_LEVEL, ValueType::kInt16},
        {EnvironmentalReverb::roomHfLevelMb, REVERB_PARAM_ROOM_HF_LEVEL, ValueType::kInt16},
        {EnvironmentalReverb::decayTimeMs, REVERB_PARAM_DECAY_TIME, ValueType::kUint32},
        {EnvironmentalReverb::decayHfRatioPm, REVERB_PARAM_DECAY_HF_RATIO, ValueType::kInt16},
        {EnvironmentalReverb::reflectionsLevelMb, REVERB_PARAM_REFLECTIONS_LEVEL,
         ValueType::kInt16},
        {EnvironmentalReverb::reflectionsDelayMs, REVERB_PARAM_REFLECTIONS_DELAY,
         ValueType::kUint32},
        {EnvironmentalReverb::levelMb, REVERB_PARAM_REVERB_LEVEL, ValueType::kInt16},
        {EnvironmentalReverb::delayMs, REVERB_PARAM_REVERB_DELAY, ValueType::kUint32},
        {EnvironmentalReverb::diffusionPm, REVERB_PARAM_DIFFUSION, ValueType::kInt16},
        {EnvironmentalReverb::densityPm, REVERB_PARAM_DENSITY, ValueType::kInt16},
        {EnvironmentalReverb::bypass, REVERB_PARAM_BYPASS, ValueType::kBool},
};

const EnvReverbField* FindField(EnvironmentalReverb::Tag tag) {
    for (const auto& field : kEnvReverbFields) {
        if (field.tag == tag) return &field;
    }
    return nullptr;
}

// Reads the integer payload of any EnvironmentalReverb variant.
int32_t IntValue(const EnvironmentalReverb& value) {
    switch (value.getTag()) {
        case EnvironmentalReverb::roomLevelMb:
            return value.get<EnvironmentalReverb::roomLevelMb>();
        case EnvironmentalReverb::roomHfLevelMb:
            return value.get<EnvironmentalReverb::roomHfLevelMb>();
        case EnvironmentalReverb::decayTimeMs:
            return value.get<EnvironmentalReverb::decayTimeMs>();
        case EnvironmentalReverb::decayHfRatioPm:
            return value.get<EnvironmentalReverb::decayHfRatioPm>();
        case EnvironmentalReverb::reflectionsLevelMb:
            return value.get<EnvironmentalReverb::reflectionsLevelMb>();
        case EnvironmentalReverb::reflectionsDelayMs:
            return value.get<EnvironmentalReverb::reflectionsDelayMs>();
        case EnvironmentalReverb::levelMb:
            return value.get<EnvironmentalReverb::levelMb>();
        case EnvironmentalReverb::delayMs:
            return value.get<EnvironmentalReverb::delayMs>();
        case EnvironmentalReverb::diffusionPm:
            return value.get<EnvironmentalReverb::diffusionPm>();
        case EnvironmentalReverb::densityPm:
            return value.get<EnvironmentalReverb::densityPm>();
        case EnvironmentalReverb::bypass:
            return value.get<EnvironmentalReverb::bypass>() ? 1 : 0;
        default:
            return 0;
    }
}

void SetIntValue(EnvironmentalReverb::Tag tag, int32_t v, EnvironmentalReverb* value) {
    switch (tag) {
        case EnvironmentalReverb::roomLevelMb:
            value->set<EnvironmentalReverb::roomLevelMb>(v);
            break;
        case EnvironmentalReverb::roomHfLevelMb:
            value->set<EnvironmentalReverb::roomHfLevelMb>(v);
            break;
        case EnvironmentalReverb::decayTimeMs:
            value->set<EnvironmentalReverb::decayTimeMs>(v);
            break;
        case EnvironmentalReverb::decayHfRatioPm:
            value->set<EnvironmentalReverb::decayHfRatioPm>(v);
            break;
        case EnvironmentalReverb::reflectionsLevelMb:
            value->set<EnvironmentalReverb::reflectionsLevelMb>(v);
            break;
        case EnvironmentalReverb::reflectionsDelayMs:
            value->set<EnvironmentalReverb::reflectionsDelayMs>(v);
            break;
        case EnvironmentalReverb::levelMb:
            value->set<EnvironmentalReverb::levelMb>(v);
            break;
        case EnvironmentalReverb::delayMs:
            value->set<EnvironmentalReverb::delayMs>(v);
            break;
        case EnvironmentalReverb::diffusionPm:
            value->set<EnvironmentalReverb::diffusionPm>(v);
            break;
        case EnvironmentalReverb::densityPm:
            value->set<EnvironmentalReverb::densityPm>(v);
            break;
        case EnvironmentalReverb::bypass:
            value->set<EnvironmentalReverb::bypass>(v != 0);
            break;
        default:
            break;
    }
}

class EnvironmentalReverbTranslator final
    : public TypedTranslator<EnvironmentalReverb, Parameter::Specific::environmentalReverb,
                             Parameter::Id::environmentalReverbTag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect,
                                const EnvironmentalReverb& value) override {
        const EnvReverbField* field = FindField(value.getTag());
        if (field == nullptr) return Unsupported("environmental reverb parameter");
        const int32_t v = IntValue(value);
        int32_t status;
        switch (field->type) {
            case ValueType::kInt16:
                status = SetSimple(effect, field->legacy_id, static_cast<int16_t>(v));
                break;
            case ValueType::kUint32:
                status = SetSimple(effect, field->legacy_id, static_cast<uint32_t>(v));
                break;
            case ValueType::kBool:
                status = SetSimple(effect, field->legacy_id, static_cast<int32_t>(v != 0));
                break;
        }
        return LegacyStatusToBinder(status, "REVERB_PARAM_*");
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, EnvironmentalReverb::Tag tag,
                                EnvironmentalReverb* value) override {
        const EnvReverbField* field = FindField(tag);
        if (field == nullptr) return Unsupported("environmental reverb parameter");
        std::optional<int32_t> v;
        switch (field->type) {
            case ValueType::kInt16:
                if (auto r = GetSimple<int16_t>(effect, field->legacy_id)) v = *r;
                break;
            case ValueType::kUint32:
                if (auto r = GetSimple<uint32_t>(effect, field->legacy_id))
                    v = static_cast<int32_t>(*r);
                break;
            case ValueType::kBool:
                if (auto r = GetSimple<int32_t>(effect, field->legacy_id)) v = *r != 0;
                break;
        }
        if (!v.has_value()) return LegacyStatusToBinder(-EINVAL, "REVERB_PARAM_*");
        SetIntValue(tag, *v, value);
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreatePresetReverbTranslator() {
    return std::make_unique<PresetReverbTranslator>();
}

std::unique_ptr<ParameterTranslator> CreateEnvironmentalReverbTranslator() {
    return std::make_unique<EnvironmentalReverbTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
