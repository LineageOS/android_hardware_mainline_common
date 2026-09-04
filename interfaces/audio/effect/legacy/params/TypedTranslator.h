/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "params/ParameterTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

// Boilerplate shared by the translators of the typed effects: unwrapping the
// Parameter::Specific / Parameter::Id unions and handling the <Effect>::vendor
// escape hatch. Subclasses implement SetTyped() / GetTyped() for the fields of
// their effect union.
//
//   Effect        the AIDL union, e.g. Equalizer
//   kSpecificTag  Parameter::Specific::equalizer
//   kIdTag        Parameter::Id::equalizerTag
template <typename Effect, Parameter::Specific::Tag kSpecificTag, Parameter::Id::Tag kIdTag>
class TypedTranslator : public ParameterTranslator {
  public:
    ndk::ScopedAStatus Set(LegacyEffectHandle& effect,
                           const Parameter::Specific& specific) override {
        if (specific.getTag() != kSpecificTag) {
            return IllegalArgument("parameter is for another effect type");
        }
        const Effect& value = specific.template get<kSpecificTag>();
        if (value.getTag() == Effect::vendor) {
            return SetVendorExtension(effect, value.template get<Effect::vendor>());
        }
        return SetTyped(effect, value);
    }

    ndk::ScopedAStatus Get(LegacyEffectHandle& effect, const Parameter::Id& id,
                           Parameter::Specific* specific) override {
        if (id.getTag() != kIdTag) {
            return IllegalArgument("parameter id is for another effect type");
        }
        const typename Effect::Id& effect_id = id.template get<kIdTag>();
        Effect value;
        if (effect_id.getTag() == Effect::Id::vendorExtensionTag) {
            VendorExtension reply;
            const auto status = GetVendorExtension(
                    effect, effect_id.template get<Effect::Id::vendorExtensionTag>(), &reply);
            if (!status.isOk()) return status;
            value.template set<Effect::vendor>(reply);
        } else if (effect_id.getTag() == Effect::Id::commonTag) {
            const auto status =
                    GetTyped(effect, effect_id.template get<Effect::Id::commonTag>(), &value);
            if (!status.isOk()) return status;
        } else {
            return GetExtra(effect, effect_id, &value);
        }
        specific->template set<kSpecificTag>(value);
        return ndk::ScopedAStatus::ok();
    }

  protected:
    virtual ndk::ScopedAStatus SetTyped(LegacyEffectHandle& effect, const Effect& value) = 0;
    virtual ndk::ScopedAStatus GetTyped(LegacyEffectHandle& effect, typename Effect::Tag tag,
                                        Effect* value) = 0;
    // Ids with additional payload variants (e.g. Virtualizer::Id::
    // speakerAnglesPayload). Unsupported by default.
    virtual ndk::ScopedAStatus GetExtra(LegacyEffectHandle& /*effect*/,
                                        const typename Effect::Id& /*id*/, Effect* /*value*/) {
        return Unsupported("parameter id variant");
    }
};

// For effect types whose typed parameters are not translated (yet): only the
// vendor escape hatch works, typed fields report EX_UNSUPPORTED_OPERATION.
template <typename Effect, Parameter::Specific::Tag kSpecificTag, Parameter::Id::Tag kIdTag>
class VendorOnlyTranslator final : public TypedTranslator<Effect, kSpecificTag, kIdTag> {
  protected:
    ndk::ScopedAStatus SetTyped(LegacyEffectHandle& /*effect*/, const Effect& /*value*/) override {
        return Unsupported("typed parameter for this effect type");
    }
    ndk::ScopedAStatus GetTyped(LegacyEffectHandle& /*effect*/, typename Effect::Tag /*tag*/,
                                Effect* /*value*/) override {
        return Unsupported("typed parameter for this effect type");
    }
};

}  // namespace aidl::android::hardware::audio::effect::legacy
