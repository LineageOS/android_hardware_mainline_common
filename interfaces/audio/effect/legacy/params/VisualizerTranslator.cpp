/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Visualizer"

#include <vector>

#include <aidl/android/hardware/audio/effect/Range.h>
#include <aidl/android/hardware/audio/effect/Visualizer.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_visualizer.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

// MAX_LATENCY_MS of the AOSP visualizer, which clamps a larger value.
constexpr int32_t kMaxLatencyMs = 3000;

class VisualizerTranslator final
    : public TypedTranslator<Visualizer, Parameter::Specific::visualizer,
                             Parameter::Id::visualizerTag> {
  public:
    void FillCapability(LegacyEffectHandle& /*effect*/, Capability* capability) override {
        Range::VisualizerRange range;
        range.min.set<Visualizer::captureSamples>(VISUALIZER_CAPTURE_SIZE_MIN);
        range.max.set<Visualizer::captureSamples>(VISUALIZER_CAPTURE_SIZE_MAX);
        Range::VisualizerRange latency_range;
        latency_range.min.set<Visualizer::latencyMs>(0);
        latency_range.max.set<Visualizer::latencyMs>(kMaxLatencyMs);
        capability->range.set<Range::visualizer>(
                std::vector<Range::VisualizerRange>{range, latency_range});
    }

    void OnOpen(LegacyEffectHandle& /*effect*/) override { latency_ms_ = 0; }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const Visualizer& value) override {
        switch (value.getTag()) {
            case Visualizer::captureSamples: {
                const int32_t samples = value.get<Visualizer::captureSamples>();
                if (samples < VISUALIZER_CAPTURE_SIZE_MIN ||
                    samples > VISUALIZER_CAPTURE_SIZE_MAX) {
                    return IllegalArgument("visualizer capture size out of range");
                }
                return LegacyStatusToBinder(SetSimple(effect, VISUALIZER_PARAM_CAPTURE_SIZE,
                                                      static_cast<uint32_t>(samples)),
                                            "VISUALIZER_PARAM_CAPTURE_SIZE");
            }
            case Visualizer::scalingMode:
                return LegacyStatusToBinder(
                        SetSimple(effect, VISUALIZER_PARAM_SCALING_MODE,
                                  static_cast<uint32_t>(value.get<Visualizer::scalingMode>())),
                        "VISUALIZER_PARAM_SCALING_MODE");
            case Visualizer::latencyMs: {
                const int32_t latency = value.get<Visualizer::latencyMs>();
                if (latency < 0 || latency > kMaxLatencyMs) {
                    return IllegalArgument("visualizer latency out of range");
                }
                auto status = LegacyStatusToBinder(
                        SetSimple(effect, VISUALIZER_PARAM_LATENCY, static_cast<uint32_t>(latency)),
                        "VISUALIZER_PARAM_LATENCY");
                if (status.isOk()) latency_ms_ = latency;
                return status;
            }
            case Visualizer::measurementMode:
                return LegacyStatusToBinder(
                        SetSimple(effect, VISUALIZER_PARAM_MEASUREMENT_MODE,
                                  static_cast<uint32_t>(value.get<Visualizer::measurementMode>())),
                        "VISUALIZER_PARAM_MEASUREMENT_MODE");
            default:
                return Unsupported("visualizer parameter");
        }
    }

    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, Visualizer::Tag tag,
                                Visualizer* value) override {
        switch (tag) {
            case Visualizer::captureSamples: {
                const auto samples = GetSimple<uint32_t>(effect, VISUALIZER_PARAM_CAPTURE_SIZE);
                if (!samples.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "VISUALIZER_PARAM_CAPTURE_SIZE");
                }
                value->set<Visualizer::captureSamples>(static_cast<int32_t>(*samples));
                return ndk::ScopedAStatus::ok();
            }
            case Visualizer::scalingMode: {
                const auto mode = GetSimple<uint32_t>(effect, VISUALIZER_PARAM_SCALING_MODE);
                if (!mode.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "VISUALIZER_PARAM_SCALING_MODE");
                }
                value->set<Visualizer::scalingMode>(static_cast<Visualizer::ScalingMode>(*mode));
                return ndk::ScopedAStatus::ok();
            }
            case Visualizer::latencyMs:
                // The library takes the latency but cannot tell it.
                value->set<Visualizer::latencyMs>(latency_ms_);
                return ndk::ScopedAStatus::ok();
            case Visualizer::measurementMode: {
                const auto mode = GetSimple<uint32_t>(effect, VISUALIZER_PARAM_MEASUREMENT_MODE);
                if (!mode.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "VISUALIZER_PARAM_MEASUREMENT_MODE");
                }
                value->set<Visualizer::measurementMode>(
                        static_cast<Visualizer::MeasurementMode>(*mode));
                return ndk::ScopedAStatus::ok();
            }
            case Visualizer::captureSampleBuffer: {
                // Proprietary command: the reply is `captureSamples` bytes of
                // unsigned 8-bit PCM.
                const auto capture_samples =
                        GetSimple<uint32_t>(effect, VISUALIZER_PARAM_CAPTURE_SIZE);
                if (!capture_samples.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "VISUALIZER_PARAM_CAPTURE_SIZE");
                }
                std::vector<uint8_t> samples(*capture_samples);
                uint32_t reply_size = static_cast<uint32_t>(samples.size());
                const int32_t status = effect.Command(VISUALIZER_CMD_CAPTURE, 0, nullptr,
                                                      &reply_size, samples.data());
                if (status != 0) return LegacyStatusToBinder(status, "VISUALIZER_CMD_CAPTURE");
                samples.resize(reply_size);
                value->set<Visualizer::captureSampleBuffer>(samples);
                return ndk::ScopedAStatus::ok();
            }
            case Visualizer::measurement: {
                int32_t reply[MEASUREMENT_COUNT] = {};
                uint32_t reply_size = sizeof(reply);
                const int32_t status =
                        effect.Command(VISUALIZER_CMD_MEASURE, 0, nullptr, &reply_size, reply);
                if (status != 0) return LegacyStatusToBinder(status, "VISUALIZER_CMD_MEASURE");
                value->set<Visualizer::measurement>(Visualizer::Measurement{
                        .rms = reply[MEASUREMENT_IDX_RMS], .peak = reply[MEASUREMENT_IDX_PEAK]});
                return ndk::ScopedAStatus::ok();
            }
            default:
                return Unsupported("visualizer parameter");
        }
    }

  private:
    int32_t latency_ms_ = 0;
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateVisualizerTranslator() {
    return std::make_unique<VisualizerTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
