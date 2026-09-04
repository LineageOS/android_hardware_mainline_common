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

class VisualizerTranslator final
    : public TypedTranslator<Visualizer, Parameter::Specific::visualizer,
                             Parameter::Id::visualizerTag> {
  public:
    void FillCapability(LegacyEffectHandle& /*effect*/, Capability* capability) override {
        Range::VisualizerRange range;
        range.min.set<Visualizer::captureSamples>(VISUALIZER_CAPTURE_SIZE_MIN);
        range.max.set<Visualizer::captureSamples>(VISUALIZER_CAPTURE_SIZE_MAX);
        capability->range.set<Range::visualizer>(std::vector<Range::VisualizerRange>{range});
    }

  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const Visualizer& value) override {
        switch (value.getTag()) {
            case Visualizer::captureSamples:
                capture_samples_ = value.get<Visualizer::captureSamples>();
                return LegacyStatusToBinder(SetSimple(effect, VISUALIZER_PARAM_CAPTURE_SIZE,
                                                      static_cast<uint32_t>(capture_samples_)),
                                            "VISUALIZER_PARAM_CAPTURE_SIZE");
            case Visualizer::scalingMode:
                return LegacyStatusToBinder(
                        SetSimple(effect, VISUALIZER_PARAM_SCALING_MODE,
                                  static_cast<uint32_t>(value.get<Visualizer::scalingMode>())),
                        "VISUALIZER_PARAM_SCALING_MODE");
            case Visualizer::latencyMs:
                return LegacyStatusToBinder(
                        SetSimple(effect, VISUALIZER_PARAM_LATENCY,
                                  static_cast<uint32_t>(value.get<Visualizer::latencyMs>())),
                        "VISUALIZER_PARAM_LATENCY");
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
                capture_samples_ = static_cast<int32_t>(*samples);
                value->set<Visualizer::captureSamples>(capture_samples_);
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
            case Visualizer::latencyMs: {
                const auto latency = GetSimple<uint32_t>(effect, VISUALIZER_PARAM_LATENCY);
                if (!latency.has_value()) {
                    return LegacyStatusToBinder(-EINVAL, "VISUALIZER_PARAM_LATENCY");
                }
                value->set<Visualizer::latencyMs>(static_cast<int32_t>(*latency));
                return ndk::ScopedAStatus::ok();
            }
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
                std::vector<uint8_t> samples(static_cast<size_t>(capture_samples_));
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
    int32_t capture_samples_ = VISUALIZER_CAPTURE_SIZE_MAX;
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateVisualizerTranslator() {
    return std::make_unique<VisualizerTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
