/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>

#include <aidl/android/hardware/audio/effect/Capability.h>
#include <aidl/android/hardware/audio/effect/Parameter.h>
#include <aidl/android/hardware/audio/effect/VendorExtension.h>
#include <aidl/android/media/audio/common/AudioUuid.h>
#include <android/binder_auto_utils.h>

#include "LegacyLibrary.h"

namespace aidl::android::hardware::audio::effect::legacy {

// Translates between the typed AIDL Parameter::Specific of one effect type and
// the legacy EFFECT_CMD_SET_PARAM / EFFECT_CMD_GET_PARAM payloads. One instance
// per effect type; stateless apart from what it queries from the legacy effect.
class ParameterTranslator {
  public:
    virtual ~ParameterTranslator() = default;

    // AIDL -> legacy.
    virtual ndk::ScopedAStatus Set(LegacyEffectHandle& effect,
                                   const Parameter::Specific& specific) = 0;
    // legacy -> AIDL. `id` selects what to read.
    virtual ndk::ScopedAStatus Get(LegacyEffectHandle& effect, const Parameter::Id& id,
                                   Parameter::Specific* specific) = 0;
    // Fills Descriptor.capability by querying a freshly created instance.
    // The default leaves the capability empty.
    virtual void FillCapability(LegacyEffectHandle& /*effect*/, Capability* /*capability*/) {}
};

// Picks the translator for an effect type UUID: a typed one for the types the
// framework converts, the generic vendor pass-through for everything else.
std::unique_ptr<ParameterTranslator> CreateTranslator(
        const ::aidl::android::media::audio::common::AudioUuid& type);

// --- Vendor extension pass-through -------------------------------------------
//
// The framework wraps whatever it does not understand as raw effect_param_t
// bytes inside a DefaultExtension: for unknown effect types in
// Parameter::Specific::vendorEffect, for known types in <Effect>::vendor. On a
// get, the request effect_param_t (parameter area filled, vsize = room for the
// value) is carried the same way in the Parameter::Id.

// EFFECT_CMD_SET_PARAM with the bytes of `extension`.
ndk::ScopedAStatus SetVendorExtension(LegacyEffectHandle& effect, const VendorExtension& extension);
// EFFECT_CMD_GET_PARAM for the request in `request`, reply in `*reply`.
ndk::ScopedAStatus GetVendorExtension(LegacyEffectHandle& effect, const VendorExtension& request,
                                      VendorExtension* reply);

// Helpers shared by the typed translators.
ndk::ScopedAStatus LegacyStatusToBinder(int32_t status, const char* what);
ndk::ScopedAStatus IllegalArgument(const char* what);
ndk::ScopedAStatus Unsupported(const char* what);

}  // namespace aidl::android::hardware::audio::effect::legacy
