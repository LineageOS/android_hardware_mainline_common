/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Equalizer"

#include <cstring>
#include <vector>

#include <aidl/android/hardware/audio/effect/Equalizer.h>
#include <aidl/android/hardware/audio/effect/Range.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_equalizer.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

// Legacy preset name length: EQ_PARAM_GET_PRESET_NAME replies a NUL
// terminated string of at most EFFECT_STRING_LEN_MAX bytes.
constexpr uint32_t kPresetNameSize = EFFECT_STRING_LEN_MAX;

class EqualizerTranslator final : public TypedTranslator<Equalizer, Parameter::Specific::equalizer,
                                                         Parameter::Id::equalizerTag> {
  public:
    void FillCapability(LegacyEffectHandle& effect, Capability* capability) override {
        // The framework reads EQ_PARAM_LEVEL_RANGE and the preset range from
        // the capability rather than asking the effect.
        const auto bands = NumBands(effect);
        if (!bands.has_value() || *bands == 0) return;
        LegacyParam range_param(sizeof(uint32_t), 2 * sizeof(int16_t));
        range_param.SetParam<uint32_t>(0, EQ_PARAM_LEVEL_RANGE);
        if (GetParam(effect, &range_param) != 0 || range_param.vsize() < 2 * sizeof(int16_t)) {
            return;
        }
        const int16_t min_level = range_param.GetValue<int16_t>(0);
        const int16_t max_level = range_param.GetValue<int16_t>(sizeof(int16_t));

        std::vector<Equalizer::BandLevel> min_levels;
        std::vector<Equalizer::BandLevel> max_levels;
        for (int32_t band = 0; band < static_cast<int32_t>(*bands); ++band) {
            min_levels.push_back({.index = band, .levelMb = min_level});
            max_levels.push_back({.index = band, .levelMb = max_level});
        }
        Range::EqualizerRange levels;
        levels.min.set<Equalizer::bandLevels>(min_levels);
        levels.max.set<Equalizer::bandLevels>(max_levels);

        std::vector<Range::EqualizerRange> ranges = {levels};
        if (const auto presets = NumPresets(effect); presets.has_value() && *presets > 0) {
            Range::EqualizerRange preset_range;
            preset_range.min.set<Equalizer::preset>(0);
            preset_range.max.set<Equalizer::preset>(static_cast<int32_t>(*presets) - 1);
            ranges.push_back(preset_range);
        }
        capability->range.set<Range::equalizer>(ranges);
        LOG(DEBUG) << __func__ << ": " << *bands << " bands, level range [" << min_level << ", "
                   << max_level << "] mB";
    }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const Equalizer& value) override {
        switch (value.getTag()) {
            case Equalizer::preset: {
                const auto preset = static_cast<uint16_t>(value.get<Equalizer::preset>());
                return LegacyStatusToBinder(SetSimple(effect, EQ_PARAM_CUR_PRESET, preset),
                                            "EQ_PARAM_CUR_PRESET");
            }
            case Equalizer::bandLevels: {
                for (const auto& band : value.get<Equalizer::bandLevels>()) {
                    const auto status =
                            LegacyStatusToBinder(SetIndexed(effect, EQ_PARAM_BAND_LEVEL, band.index,
                                                            static_cast<int16_t>(band.levelMb)),
                                                 "EQ_PARAM_BAND_LEVEL");
                    if (!status.isOk()) return status;
                }
                return ndk::ScopedAStatus::ok();
            }
            default:
                // centerFreqMh, bandFrequencies and presets are read-only.
                return Unsupported("read-only equalizer parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, Equalizer::Tag tag,
                                Equalizer* value) override {
        switch (tag) {
            case Equalizer::preset: {
                const auto preset = GetSimple<uint16_t>(effect, EQ_PARAM_CUR_PRESET);
                if (!preset.has_value())
                    return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_CUR_PRESET");
                // Legacy reports 0xffff for "custom".
                value->set<Equalizer::preset>(*preset == 0xffff ? -1
                                                                : static_cast<int32_t>(*preset));
                return ndk::ScopedAStatus::ok();
            }
            case Equalizer::bandLevels: {
                std::vector<Equalizer::BandLevel> levels;
                const auto bands = NumBands(effect);
                if (!bands.has_value()) return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_NUM_BANDS");
                for (int32_t band = 0; band < static_cast<int32_t>(*bands); ++band) {
                    const auto level = GetIndexed<int16_t>(effect, EQ_PARAM_BAND_LEVEL, band);
                    if (!level.has_value()) {
                        return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_BAND_LEVEL");
                    }
                    levels.push_back({.index = band, .levelMb = *level});
                }
                value->set<Equalizer::bandLevels>(levels);
                return ndk::ScopedAStatus::ok();
            }
            case Equalizer::centerFreqMh: {
                std::vector<int32_t> freqs;
                const auto bands = NumBands(effect);
                if (!bands.has_value()) return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_NUM_BANDS");
                for (int32_t band = 0; band < static_cast<int32_t>(*bands); ++band) {
                    const auto freq = GetIndexed<int32_t>(effect, EQ_PARAM_CENTER_FREQ, band);
                    if (!freq.has_value()) {
                        return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_CENTER_FREQ");
                    }
                    freqs.push_back(*freq);
                }
                value->set<Equalizer::centerFreqMh>(freqs);
                return ndk::ScopedAStatus::ok();
            }
            case Equalizer::bandFrequencies: {
                std::vector<Equalizer::BandFrequency> ranges;
                const auto bands = NumBands(effect);
                if (!bands.has_value()) return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_NUM_BANDS");
                for (int32_t band = 0; band < static_cast<int32_t>(*bands); ++band) {
                    LegacyParam param(2 * sizeof(uint32_t), 2 * sizeof(int32_t));
                    param.SetParam<uint32_t>(0, EQ_PARAM_BAND_FREQ_RANGE);
                    param.SetParam<int32_t>(1, band);
                    if (GetParam(effect, &param) != 0 || param.vsize() < 2 * sizeof(int32_t)) {
                        return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_BAND_FREQ_RANGE");
                    }
                    ranges.push_back({.index = band,
                                      .minMh = param.GetValue<int32_t>(0),
                                      .maxMh = param.GetValue<int32_t>(sizeof(int32_t))});
                }
                value->set<Equalizer::bandFrequencies>(ranges);
                return ndk::ScopedAStatus::ok();
            }
            case Equalizer::presets: {
                std::vector<Equalizer::Preset> presets;
                const auto count = NumPresets(effect);
                if (!count.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "EQ_PARAM_GET_NUM_OF_PRESETS");
                }
                for (int32_t preset = 0; preset < static_cast<int32_t>(*count); ++preset) {
                    LegacyParam param(2 * sizeof(uint32_t), kPresetNameSize);
                    param.SetParam<uint32_t>(0, EQ_PARAM_GET_PRESET_NAME);
                    param.SetParam<int32_t>(1, preset);
                    std::string name;
                    if (GetParam(effect, &param) == 0 && param.vsize() > 0) {
                        const auto* chars = reinterpret_cast<const char*>(param.bytes().data()) +
                                            sizeof(effect_param_t) +
                                            LegacyParam::Padded(param.psize());
                        name.assign(chars, strnlen(chars, param.vsize()));
                    }
                    presets.push_back({.index = preset, .name = name});
                }
                value->set<Equalizer::presets>(presets);
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("equalizer parameter");
        }
    }

  private:
    static std::optional<uint16_t> NumBands(LegacyEffectHandle& effect) {
        return GetSimple<uint16_t>(effect, EQ_PARAM_NUM_BANDS);
    }
    static std::optional<uint16_t> NumPresets(LegacyEffectHandle& effect) {
        return GetSimple<uint16_t>(effect, EQ_PARAM_GET_NUM_OF_PRESETS);
    }
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateEqualizerTranslator() {
    return std::make_unique<EqualizerTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
