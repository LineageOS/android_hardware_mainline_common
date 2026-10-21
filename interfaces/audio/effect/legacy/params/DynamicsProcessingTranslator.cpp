/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_DynamicsProcessing"

// DynamicsProcessing. Mirrors AidlConversionDynamicsProcessing.cpp of
// libaudiohal, which turns the legacy parameters into the AIDL ones.
//
// The legacy effect configures one channel, or one band of a channel, per
// parameter and has no way to tell which of them a client set. The AIDL side
// sets and gets lists, so the translator keeps what it was given and answers a
// get with it. The effect only gets values it can handle: a stage that is not in
// use has no bands for it.

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <aidl/android/hardware/audio/effect/DynamicsProcessing.h>
#include <aidl/android/hardware/audio/effect/Range.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_dynamicsprocessing.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

using DP = DynamicsProcessing;

constexpr size_t kEngineValues = 9;
constexpr float kDefaultProcessingDurationMs = 10;
constexpr size_t kChannelValues = 3;
constexpr size_t kEqBandValues = 3;
constexpr size_t kMbcBandValues = 11;
constexpr size_t kLimiterValues = 8;

// Same cutoff range as the AOSP software implementation.
constexpr float kMinCutoffHz = 220;
constexpr float kMaxCutoffHz = 20000;

// A parameter of `args.size()` words after the command, and room for `values`
// words of value. Both are int32_t / float sized.
LegacyParam MakeParam(uint32_t command, std::initializer_list<int32_t> args, size_t values) {
    LegacyParam param(static_cast<uint32_t>(1 + args.size()) * sizeof(int32_t),
                      static_cast<uint32_t>(values) * sizeof(int32_t));
    param.SetParam<uint32_t>(0, command);
    size_t index = 1;
    for (const int32_t arg : args) param.SetParam<int32_t>(index++, arg);
    return param;
}

// Value word `index` of a parameter.
template <typename T>
void Put(LegacyParam& param, size_t index, T value) {
    static_assert(sizeof(T) == sizeof(int32_t));
    param.SetValue<T>(index * sizeof(int32_t), value);
}

template <typename T>
T Take(const LegacyParam& param, size_t index) {
    static_assert(sizeof(T) == sizeof(int32_t));
    return param.GetValue<T>(index * sizeof(int32_t));
}

std::optional<LegacyParam> Query(LegacyEffectHandle& effect, uint32_t command,
                                 std::initializer_list<int32_t> args, size_t values) {
    LegacyParam param = MakeParam(command, args, values);
    if (GetParam(effect, &param) != 0 || param.vsize() < values * sizeof(int32_t)) {
        return std::nullopt;
    }
    return param;
}

std::optional<DP::EngineArchitecture> QueryEngine(LegacyEffectHandle& effect) {
    const auto reply = Query(effect, DP_PARAM_ENGINE_ARCHITECTURE, {}, kEngineValues);
    if (!reply.has_value()) return std::nullopt;
    DP::EngineArchitecture engine;
    engine.resolutionPreference = static_cast<DP::ResolutionPreference>(Take<int32_t>(*reply, 0));
    engine.preferredProcessingDurationMs = Take<float>(*reply, 1);
    engine.preEqStage = {.inUse = Take<int32_t>(*reply, 2) != 0,
                         .bandCount = Take<int32_t>(*reply, 3)};
    engine.mbcStage = {.inUse = Take<int32_t>(*reply, 4) != 0,
                       .bandCount = Take<int32_t>(*reply, 5)};
    engine.postEqStage = {.inUse = Take<int32_t>(*reply, 6) != 0,
                          .bandCount = Take<int32_t>(*reply, 7)};
    engine.limiterInUse = Take<int32_t>(*reply, 8) != 0;
    return engine;
}

std::optional<int32_t> QueryChannelCount(LegacyEffectHandle& effect) {
    const auto reply = Query(effect, DP_PARAM_GET_CHANNEL_COUNT, {}, 1);
    if (!reply.has_value()) return std::nullopt;
    return Take<int32_t>(*reply, 0);
}

// The effect allocates for every band of every channel; a client has no use
// for more than a handful, so do not let one ask for gigabytes.
constexpr int32_t kMaxBandCount = 128;

bool ValidStage(const DP::StageEnablement& stage) {
    return stage.bandCount <= kMaxBandCount;
}

class DynamicsProcessingTranslator final
    : public TypedTranslator<DP, Parameter::Specific::dynamicsProcessing,
                             Parameter::Id::dynamicsProcessingTag> {
  public:
    void FillCapability(LegacyEffectHandle& /*effect*/, Capability* capability) override {
        DP::EqBandConfig min = {.channel = 0,
                                .band = 0,
                                .enable = false,
                                .cutoffFrequencyHz = kMinCutoffHz,
                                .gainDb = std::numeric_limits<float>::lowest()};
        DP::EqBandConfig max = {.channel = std::numeric_limits<int32_t>::max(),
                                .band = std::numeric_limits<int32_t>::max(),
                                .enable = true,
                                .cutoffFrequencyHz = kMaxCutoffHz,
                                .gainDb = std::numeric_limits<float>::max()};
        Range::DynamicsProcessingRange pre = {.min = DP::make<DP::preEqBand>({min}),
                                              .max = DP::make<DP::preEqBand>({max})};
        Range::DynamicsProcessingRange post = {.min = DP::make<DP::postEqBand>({min}),
                                               .max = DP::make<DP::postEqBand>({max})};
        // The effect rejects an MBC cutoff above half the sampling rate. Take the
        // same upper limit as for the equalizer bands.
        constexpr float kLowest = std::numeric_limits<float>::lowest();
        constexpr float kHighest = std::numeric_limits<float>::max();
        DP::MbcBandConfig mbc_min = {.channel = 0,
                                     .band = 0,
                                     .enable = false,
                                     .cutoffFrequencyHz = 0,
                                     .attackTimeMs = kLowest,
                                     .releaseTimeMs = kLowest,
                                     .ratio = kLowest,
                                     .thresholdDb = kLowest,
                                     .kneeWidthDb = kLowest,
                                     .noiseGateThresholdDb = kLowest,
                                     .expanderRatio = kLowest,
                                     .preGainDb = kLowest,
                                     .postGainDb = kLowest};
        DP::MbcBandConfig mbc_max = {.channel = std::numeric_limits<int32_t>::max(),
                                     .band = std::numeric_limits<int32_t>::max(),
                                     .enable = true,
                                     .cutoffFrequencyHz = kMaxCutoffHz,
                                     .attackTimeMs = kHighest,
                                     .releaseTimeMs = kHighest,
                                     .ratio = kHighest,
                                     .thresholdDb = kHighest,
                                     .kneeWidthDb = kHighest,
                                     .noiseGateThresholdDb = kHighest,
                                     .expanderRatio = kHighest,
                                     .preGainDb = kHighest,
                                     .postGainDb = kHighest};
        Range::DynamicsProcessingRange mbc = {.min = DP::make<DP::mbcBand>({mbc_min}),
                                              .max = DP::make<DP::mbcBand>({mbc_max})};
        capability->range.set<Range::dynamicsProcessing>(
                std::vector<Range::DynamicsProcessingRange>{pre, post, mbc});
    }

    void OnOpen(LegacyEffectHandle& effect) override {
        Forget();
        // The effect has no engine until one is configured and then fails to
        // process. Start with one that has no stage in use.
        LegacyParam param = MakeParam(DP_PARAM_ENGINE_ARCHITECTURE, {}, kEngineValues);
        Put<int32_t>(param, 0, VARIANT_FAVOR_FREQUENCY_RESOLUTION);
        Put<float>(param, 1, kDefaultProcessingDurationMs);
        for (size_t i = 2; i < kEngineValues; ++i) Put<int32_t>(param, i, 0);
        if (const int32_t status = SetParam(effect, param); status != 0) {
            LOG(WARNING) << __func__ << ": default engine failed: " << status;
        }
    }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const DP& value) override {
        switch (value.getTag()) {
            case DP::engineArchitecture:
                return SetEngine(effect, value.get<DP::engineArchitecture>());
            case DP::preEq:
                return SetChannels(effect, DP_PARAM_PRE_EQ, value.get<DP::preEq>(), &pre_eq_);
            case DP::postEq:
                return SetChannels(effect, DP_PARAM_POST_EQ, value.get<DP::postEq>(), &post_eq_);
            case DP::mbc:
                return SetChannels(effect, DP_PARAM_MBC, value.get<DP::mbc>(), &mbc_);
            case DP::preEqBand:
                return SetEqBands(effect, DP_PARAM_PRE_EQ, DP_PARAM_PRE_EQ_BAND,
                                  value.get<DP::preEqBand>(), &pre_eq_bands_);
            case DP::postEqBand:
                return SetEqBands(effect, DP_PARAM_POST_EQ, DP_PARAM_POST_EQ_BAND,
                                  value.get<DP::postEqBand>(), &post_eq_bands_);
            case DP::mbcBand:
                return SetMbcBands(effect, value.get<DP::mbcBand>());
            case DP::limiter:
                return SetLimiters(effect, value.get<DP::limiter>());
            case DP::inputGain:
                return SetInputGains(effect, value.get<DP::inputGain>());
            default:
                return Unsupported("dynamics processing parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, DP::Tag tag, DP* value) override {
        switch (tag) {
            case DP::engineArchitecture: {
                const auto engine = CurrentEngine(effect);
                if (!engine.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "DP_PARAM_ENGINE_ARCHITECTURE");
                }
                value->set<DP::engineArchitecture>(*engine);
                return ndk::ScopedAStatus::ok();
            }
            case DP::preEq:
                value->set<DP::preEq>(Values(pre_eq_));
                return ndk::ScopedAStatus::ok();
            case DP::postEq:
                value->set<DP::postEq>(Values(post_eq_));
                return ndk::ScopedAStatus::ok();
            case DP::mbc:
                value->set<DP::mbc>(Values(mbc_));
                return ndk::ScopedAStatus::ok();
            case DP::preEqBand:
                value->set<DP::preEqBand>(Values(pre_eq_bands_));
                return ndk::ScopedAStatus::ok();
            case DP::postEqBand:
                value->set<DP::postEqBand>(Values(post_eq_bands_));
                return ndk::ScopedAStatus::ok();
            case DP::mbcBand:
                value->set<DP::mbcBand>(Values(mbc_bands_));
                return ndk::ScopedAStatus::ok();
            case DP::limiter:
                value->set<DP::limiter>(Values(limiters_));
                return ndk::ScopedAStatus::ok();
            case DP::inputGain:
                value->set<DP::inputGain>(Values(input_gains_));
                return ndk::ScopedAStatus::ok();
            default:
                return Unsupported("dynamics processing parameter");
        }
    }

  private:
    using Key = std::pair<int32_t, int32_t>;  // channel, band

    template <typename K, typename V>
    static std::vector<V> Values(const std::map<K, V>& configs) {
        std::vector<V> values;
        for (const auto& [key, value] : configs) values.push_back(value);
        return values;
    }

    // The stage as the effect gets it: it allocates for bandCount bands of
    // every channel, and takes the count as unsigned, so a stage without bands
    // or with a negative count is an unused stage with no band.
    static DP::StageEnablement LibStage(const DP::StageEnablement& stage) {
        const bool in_use = stage.inUse && stage.bandCount > 0;
        return {.inUse = in_use, .bandCount = in_use ? stage.bandCount : 0};
    }

    // The engine the client set, or the one the effect starts with.
    std::optional<DP::EngineArchitecture> CurrentEngine(LegacyEffectHandle& effect) {
        return engine_.has_value() ? engine_ : QueryEngine(effect);
    }

    // The stage a channel command belongs to.
    static const DP::StageEnablement& Stage(const DP::EngineArchitecture& engine,
                                            uint32_t channel_command) {
        switch (channel_command) {
            case DP_PARAM_PRE_EQ:
                return engine.preEqStage;
            case DP_PARAM_POST_EQ:
                return engine.postEqStage;
            default:
                return engine.mbcStage;
        }
    }

    // The engine and channel count that the channel and band indices are
    // checked against.
    struct Limits {
        DP::EngineArchitecture engine;
        int32_t channels;
    };

    std::optional<Limits> GetLimits(LegacyEffectHandle& effect) {
        const auto engine = CurrentEngine(effect);
        const auto channels = QueryChannelCount(effect);
        if (!engine.has_value() || !channels.has_value()) return std::nullopt;
        return Limits{*engine, *channels};
    }

    // --- set -----------------------------------------------------------------

    ndk::ScopedAStatus SetEngine(LegacyEffectHandle& effect, const DP::EngineArchitecture& engine) {
        if (!ValidStage(engine.preEqStage) || !ValidStage(engine.postEqStage) ||
            !ValidStage(engine.mbcStage)) {
            return IllegalArgument("dynamics processing engine architecture");
        }
        const DP::StageEnablement pre = LibStage(engine.preEqStage);
        const DP::StageEnablement mbc = LibStage(engine.mbcStage);
        const DP::StageEnablement post = LibStage(engine.postEqStage);
        // The effect implements the frequency resolution variant only.
        LegacyParam param = MakeParam(DP_PARAM_ENGINE_ARCHITECTURE, {}, kEngineValues);
        Put<int32_t>(param, 0, VARIANT_FAVOR_FREQUENCY_RESOLUTION);
        Put<float>(param, 1, std::max(engine.preferredProcessingDurationMs, 0.0f));
        Put<int32_t>(param, 2, pre.inUse);
        Put<int32_t>(param, 3, pre.bandCount);
        Put<int32_t>(param, 4, mbc.inUse);
        Put<int32_t>(param, 5, mbc.bandCount);
        Put<int32_t>(param, 6, post.inUse);
        Put<int32_t>(param, 7, post.bandCount);
        Put<int32_t>(param, 8, engine.limiterInUse);
        auto status = LegacyStatusToBinder(SetParam(effect, param), "DP_PARAM_ENGINE_ARCHITECTURE");
        if (!status.isOk()) return status;
        // What the client set is what it reads back. The effect starts over
        // with a new engine.
        Forget();
        engine_ = engine;
        return ndk::ScopedAStatus::ok();
    }

    void Forget() {
        engine_.reset();
        pre_eq_.clear();
        post_eq_.clear();
        mbc_.clear();
        pre_eq_bands_.clear();
        post_eq_bands_.clear();
        mbc_bands_.clear();
        limiters_.clear();
        input_gains_.clear();
    }

    ndk::ScopedAStatus SetChannels(LegacyEffectHandle& effect, uint32_t command,
                                   const std::vector<DP::ChannelConfig>& configs,
                                   std::map<int32_t, DP::ChannelConfig>* stored) {
        const auto limits = GetLimits(effect);
        if (!limits.has_value())
            return LegacyStatusToBinder(-EINVAL, "DP_PARAM_ENGINE_ARCHITECTURE");
        std::set<int32_t> seen;
        for (const auto& config : configs) {
            if (config.channel < 0 || config.channel >= limits->channels ||
                !seen.insert(config.channel).second) {
                return IllegalArgument("dynamics processing channel");
            }
        }
        const DP::StageEnablement stage = LibStage(Stage(limits->engine, command));
        for (const auto& config : configs) {
            LegacyParam param = MakeParam(command, {config.channel}, kChannelValues);
            Put<int32_t>(param, 0, stage.inUse);
            Put<int32_t>(param, 1, config.enable);
            Put<int32_t>(param, 2, stage.bandCount);
            auto status = LegacyStatusToBinder(SetParam(effect, param), "DP_PARAM_*_CHANNEL");
            if (!status.isOk()) return status;
            (*stored)[config.channel] = config;
        }
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus SetEqBands(LegacyEffectHandle& effect, uint32_t channel_command,
                                  uint32_t band_command, const std::vector<DP::EqBandConfig>& bands,
                                  std::map<Key, DP::EqBandConfig>* stored) {
        const auto limits = GetLimits(effect);
        if (!limits.has_value())
            return LegacyStatusToBinder(-EINVAL, "DP_PARAM_ENGINE_ARCHITECTURE");
        const DP::StageEnablement& stage = Stage(limits->engine, channel_command);
        std::set<Key> seen;
        for (const auto& band : bands) {
            if (band.channel < 0 || band.channel >= limits->channels || band.band < 0 ||
                band.band >= stage.bandCount || !seen.insert({band.channel, band.band}).second ||
                !(band.cutoffFrequencyHz >= kMinCutoffHz &&
                  band.cutoffFrequencyHz <= kMaxCutoffHz) ||
                !std::isfinite(band.gainDb)) {
                return IllegalArgument("dynamics processing eq band");
            }
        }
        for (const auto& band : bands) {
            LegacyParam param = MakeParam(band_command, {band.channel, band.band}, kEqBandValues);
            Put<int32_t>(param, 0, band.enable);
            Put<float>(param, 1, band.cutoffFrequencyHz);
            Put<float>(param, 2, band.gainDb);
            auto status = LegacyStatusToBinder(SetParam(effect, param), "DP_PARAM_*_EQ_BAND");
            if (!status.isOk()) return status;
            (*stored)[{band.channel, band.band}] = band;
        }
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus SetMbcBands(LegacyEffectHandle& effect,
                                   const std::vector<DP::MbcBandConfig>& bands) {
        const auto limits = GetLimits(effect);
        if (!limits.has_value())
            return LegacyStatusToBinder(-EINVAL, "DP_PARAM_ENGINE_ARCHITECTURE");
        std::set<Key> seen;
        for (const auto& band : bands) {
            if (band.channel < 0 || band.channel >= limits->channels || band.band < 0 ||
                band.band >= limits->engine.mbcStage.bandCount ||
                !seen.insert({band.channel, band.band}).second ||
                !(band.cutoffFrequencyHz >= 0 && band.cutoffFrequencyHz <= kMaxCutoffHz)) {
                return IllegalArgument("dynamics processing mbc band");
            }
        }
        for (const auto& band : bands) {
            LegacyParam param =
                    MakeParam(DP_PARAM_MBC_BAND, {band.channel, band.band}, kMbcBandValues);
            Put<int32_t>(param, 0, band.enable);
            Put<float>(param, 1, band.cutoffFrequencyHz);
            Put<float>(param, 2, band.attackTimeMs);
            Put<float>(param, 3, band.releaseTimeMs);
            Put<float>(param, 4, band.ratio);
            Put<float>(param, 5, band.thresholdDb);
            Put<float>(param, 6, band.kneeWidthDb);
            Put<float>(param, 7, band.noiseGateThresholdDb);
            Put<float>(param, 8, band.expanderRatio);
            Put<float>(param, 9, band.preGainDb);
            Put<float>(param, 10, band.postGainDb);
            auto status = LegacyStatusToBinder(SetParam(effect, param), "DP_PARAM_MBC_BAND");
            if (!status.isOk()) return status;
            mbc_bands_[{band.channel, band.band}] = band;
        }
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus SetLimiters(LegacyEffectHandle& effect,
                                   const std::vector<DP::LimiterConfig>& limiters) {
        const auto limits = GetLimits(effect);
        if (!limits.has_value())
            return LegacyStatusToBinder(-EINVAL, "DP_PARAM_ENGINE_ARCHITECTURE");
        std::set<int32_t> seen;
        for (const auto& limiter : limiters) {
            if (limiter.channel < 0 || limiter.channel >= limits->channels ||
                !seen.insert(limiter.channel).second) {
                return IllegalArgument("dynamics processing limiter");
            }
        }
        for (const auto& limiter : limiters) {
            LegacyParam param = MakeParam(DP_PARAM_LIMITER, {limiter.channel}, kLimiterValues);
            Put<int32_t>(param, 0, limits->engine.limiterInUse);
            Put<int32_t>(param, 1, limiter.enable);
            Put<int32_t>(param, 2, limiter.linkGroup);
            Put<float>(param, 3, limiter.attackTimeMs);
            Put<float>(param, 4, limiter.releaseTimeMs);
            Put<float>(param, 5, limiter.ratio);
            Put<float>(param, 6, limiter.thresholdDb);
            Put<float>(param, 7, limiter.postGainDb);
            auto status = LegacyStatusToBinder(SetParam(effect, param), "DP_PARAM_LIMITER");
            if (!status.isOk()) return status;
            limiters_[limiter.channel] = limiter;
        }
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus SetInputGains(LegacyEffectHandle& effect,
                                     const std::vector<DP::InputGain>& gains) {
        const auto channels = QueryChannelCount(effect);
        if (!channels.has_value()) {
            return LegacyStatusToBinder(-EINVAL, "DP_PARAM_GET_CHANNEL_COUNT");
        }
        for (const auto& gain : gains) {
            if (gain.channel < 0 || gain.channel >= *channels) {
                return IllegalArgument("dynamics processing input gain channel");
            }
        }
        for (const auto& gain : gains) {
            LegacyParam param = MakeParam(DP_PARAM_INPUT_GAIN, {gain.channel}, 1);
            Put<float>(param, 0, gain.gainDb);
            auto status = LegacyStatusToBinder(SetParam(effect, param), "DP_PARAM_INPUT_GAIN");
            if (!status.isOk()) return status;
            input_gains_[gain.channel] = gain;
        }
        return ndk::ScopedAStatus::ok();
    }

    std::optional<DP::EngineArchitecture> engine_;
    // What the client set, by channel (or channel and band).
    std::map<int32_t, DP::ChannelConfig> pre_eq_;
    std::map<int32_t, DP::ChannelConfig> post_eq_;
    std::map<int32_t, DP::ChannelConfig> mbc_;
    std::map<Key, DP::EqBandConfig> pre_eq_bands_;
    std::map<Key, DP::EqBandConfig> post_eq_bands_;
    std::map<Key, DP::MbcBandConfig> mbc_bands_;
    std::map<int32_t, DP::LimiterConfig> limiters_;
    std::map<int32_t, DP::InputGain> input_gains_;
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateDynamicsProcessingTranslator() {
    return std::make_unique<DynamicsProcessingTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
