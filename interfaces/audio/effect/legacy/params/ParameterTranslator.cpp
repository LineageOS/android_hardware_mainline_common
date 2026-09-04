/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Translator"

#include "params/ParameterTranslator.h"

#include <aidl/android/hardware/audio/effect/DefaultExtension.h>
#include <android-base/logging.h>
#include <system/audio_effects/effect_uuid.h>

#include "params/LegacyParam.h"
#include "params/Translators.h"
#include "params/TypedTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

using ::aidl::android::media::audio::common::AudioUuid;

// --- status helpers ----------------------------------------------------------

ndk::ScopedAStatus IllegalArgument(const char* what) {
    LOG(WARNING) << "illegal argument: " << what;
    return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT, what);
}

ndk::ScopedAStatus Unsupported(const char* what) {
    LOG(DEBUG) << "unsupported: " << what;
    return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_UNSUPPORTED_OPERATION, what);
}

ndk::ScopedAStatus LegacyStatusToBinder(int32_t status, const char* what) {
    if (status == 0) return ndk::ScopedAStatus::ok();
    LOG(WARNING) << what << " failed with legacy status " << status;
    switch (status) {
        case -EINVAL:
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT, what);
        case -ENOSYS:
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_UNSUPPORTED_OPERATION, what);
        default:
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_STATE, what);
    }
}

// --- vendor extension --------------------------------------------------------

namespace {

std::optional<std::vector<uint8_t>> BytesOf(const VendorExtension& extension) {
    std::optional<DefaultExtension> default_extension;
    if (extension.extension.getParcelable(&default_extension) != STATUS_OK ||
        !default_extension.has_value()) {
        LOG(WARNING) << __func__ << ": vendor extension does not carry a DefaultExtension";
        return std::nullopt;
    }
    return default_extension->bytes;
}

VendorExtension MakeExtension(std::vector<uint8_t> bytes) {
    DefaultExtension default_extension;
    default_extension.bytes = std::move(bytes);
    VendorExtension extension;
    extension.extension.setParcelable(default_extension);
    return extension;
}

}  // namespace

ndk::ScopedAStatus SetVendorExtension(LegacyEffectHandle& effect,
                                      const VendorExtension& extension) {
    const auto bytes = BytesOf(extension);
    if (!bytes.has_value()) return IllegalArgument("vendor parameter without payload");
    LegacyParam param(*bytes);
    if (!param.IsValid()) return IllegalArgument("malformed effect_param_t");
    return LegacyStatusToBinder(SetParam(effect, param), "vendor SET_PARAM");
}

ndk::ScopedAStatus GetVendorExtension(LegacyEffectHandle& effect, const VendorExtension& request,
                                      VendorExtension* reply) {
    const auto bytes = BytesOf(request);
    if (!bytes.has_value()) return IllegalArgument("vendor parameter request without payload");
    LegacyParam param(*bytes);
    if (!param.IsValid()) return IllegalArgument("malformed effect_param_t request");
    auto status = LegacyStatusToBinder(GetParam(effect, &param), "vendor GET_PARAM");
    if (!status.isOk()) return status;
    *reply = MakeExtension(param.bytes());
    return ndk::ScopedAStatus::ok();
}

// --- generic translator for unknown effect types -----------------------------

namespace {

class VendorTranslator final : public ParameterTranslator {
  public:
    ndk::ScopedAStatus Set(LegacyEffectHandle& effect,
                           const Parameter::Specific& specific) override {
        if (specific.getTag() != Parameter::Specific::vendorEffect) {
            return IllegalArgument("typed parameter for a vendor effect");
        }
        return SetVendorExtension(effect, specific.get<Parameter::Specific::vendorEffect>());
    }

    ndk::ScopedAStatus Get(LegacyEffectHandle& effect, const Parameter::Id& id,
                           Parameter::Specific* specific) override {
        if (id.getTag() != Parameter::Id::vendorEffectTag) {
            return IllegalArgument("typed parameter id for a vendor effect");
        }
        VendorExtension reply;
        auto status = GetVendorExtension(effect, id.get<Parameter::Id::vendorEffectTag>(), &reply);
        if (!status.isOk()) return status;
        specific->set<Parameter::Specific::vendorEffect>(reply);
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace

std::unique_ptr<ParameterTranslator> CreateTranslator(const AudioUuid& type) {
    if (type == getEffectTypeUuidEqualizer()) return CreateEqualizerTranslator();
    if (type == getEffectTypeUuidBassBoost()) return CreateBassBoostTranslator();
    if (type == getEffectTypeUuidVirtualizer()) return CreateVirtualizerTranslator();
    if (type == getEffectTypeUuidPresetReverb()) return CreatePresetReverbTranslator();
    if (type == getEffectTypeUuidEnvReverb()) return CreateEnvironmentalReverbTranslator();
    if (type == getEffectTypeUuidLoudnessEnhancer()) return CreateLoudnessEnhancerTranslator();
    if (type == getEffectTypeUuidDownmix()) return CreateDownmixTranslator();
    if (type == getEffectTypeUuidAcousticEchoCanceler()) return CreateAecTranslator();
    if (type == getEffectTypeUuidNoiseSuppression()) return CreateNoiseSuppressionTranslator();
    if (type == getEffectTypeUuidAutomaticGainControlV1()) return CreateAgc1Translator();
    if (type == getEffectTypeUuidAutomaticGainControlV2()) return CreateAgc2Translator();
    if (type == getEffectTypeUuidVisualizer()) return CreateVisualizerTranslator();
    // Typed effects whose parameter sets are not translated: only the vendor
    // escape hatch is available. The framework itself uses the vendor path for
    // the "volume" type.
    if (type == getEffectTypeUuidDynamicsProcessing()) {
        return std::make_unique<
                VendorOnlyTranslator<DynamicsProcessing, Parameter::Specific::dynamicsProcessing,
                                     Parameter::Id::dynamicsProcessingTag>>();
    }
    if (type == getEffectTypeUuidHapticGenerator()) {
        return std::make_unique<
                VendorOnlyTranslator<HapticGenerator, Parameter::Specific::hapticGenerator,
                                     Parameter::Id::hapticGeneratorTag>>();
    }
    if (type == getEffectTypeUuidSpatializer()) {
        return std::make_unique<VendorOnlyTranslator<Spatializer, Parameter::Specific::spatializer,
                                                     Parameter::Id::spatializerTag>>();
    }
    if (type == getEffectTypeUuidVolume()) {
        return std::make_unique<VendorOnlyTranslator<Volume, Parameter::Specific::volume,
                                                     Parameter::Id::volumeTag>>();
    }
    return std::make_unique<VendorTranslator>();
}

}  // namespace aidl::android::hardware::audio::effect::legacy
