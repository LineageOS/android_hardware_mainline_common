/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <string>

#include <effect-impl/EffectImpl.h>

#include "LegacyEffectContext.h"
#include "LegacyLibrary.h"
#include "params/ParameterTranslator.h"

namespace aidl::android::hardware::audio::effect::legacy {

// IEffect implementation backed by one legacy effect instance. The FMQ / state
// machine plumbing comes from the example HAL's EffectImpl; this class creates
// the legacy instance on open(), translates parameters and runs process().
class LegacyEffect final : public EffectImpl {
  public:
    LegacyEffect(std::shared_ptr<LegacyLibrary> library, const effect_uuid_t& uuid,
                 Descriptor descriptor);
    ~LegacyEffect() override;

    // EffectImpl
    ndk::ScopedAStatus getDescriptor(Descriptor* descriptor) override;
    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific)
            REQUIRES(mImplMutex) override;
    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id, Parameter::Specific* specific)
            REQUIRES(mImplMutex) override;
    std::string getEffectName() override { return descriptor_.common.name; }
    std::shared_ptr<EffectContext> createContext(const Parameter::Common& common)
            REQUIRES(mImplMutex) override;
    RetCode releaseContext() REQUIRES(mImplMutex) override;
    IEffect::Status effectProcessImpl(float* in, float* out, int samples)
            REQUIRES(mImplMutex) override;

  private:
    bool IsInputEffect() const;

    const std::shared_ptr<LegacyLibrary> library_;
    const effect_uuid_t uuid_;
    const Descriptor descriptor_;
    const std::unique_ptr<ParameterTranslator> translator_;
    std::shared_ptr<LegacyEffectContext> context_ GUARDED_BY(mImplMutex);
};

}  // namespace aidl::android::hardware::audio::effect::legacy
