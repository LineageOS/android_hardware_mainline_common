/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <string>

#include <aidl/android/hardware/audio/core/BnConfig.h>
#include <core-impl/AudioPolicyConfigXmlConverter.h>
#include <core-impl/EngineConfigXmlConverter.h>

namespace aidl::android::hardware::audio::core::mainline {

// IConfig implementation. The audio policy engine configuration (product
// strategies and volume curves) is read from an XML file, looked up in this
// order:
//   1. audio_policy_engine_configuration.xml in the etc/ directory of the
//      APEX that contains this binary (the generic copy shipped with the HAL);
//   2. the usual /odm/etc, /vendor/etc, /system/etc locations, so that a
//      device can override the generic file.
// The surround sound configuration is the framework default.
class MainlineConfig final : public BnConfig {
  public:
    MainlineConfig();

  private:
    ndk::ScopedAStatus getSurroundSoundConfig(SurroundSoundConfig* _aidl_return) override;
    ndk::ScopedAStatus getEngineConfig(
            ::aidl::android::media::audio::common::AudioHalEngineConfig* _aidl_return) override;

    static std::string FindEngineConfigFile();

    internal::EngineConfigXmlConverter engine_converter_;
};

}  // namespace aidl::android::hardware::audio::core::mainline
