/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Config"

#include "MainlineConfig.h"

#include <unistd.h>

#include <Log.h>
#include <android/apexsupport.h>
#include <system/audio_config.h>

namespace aidl::android::hardware::audio::core::mainline {

using ::aidl::android::media::audio::common::AudioHalEngineConfig;

namespace {

constexpr const char* kEngineConfigFileName = "audio_policy_engine_configuration.xml";

}  // namespace

std::string MainlineConfig::FindEngineConfigFile() {
    AApexInfo* apex_info = nullptr;
    if (AApexInfo_create(&apex_info) == AAPEXINFO_OK && apex_info != nullptr) {
        const std::string candidate = std::string("/apex/") + AApexInfo_getName(apex_info) +
                                      "/etc/" + kEngineConfigFileName;
        AApexInfo_destroy(apex_info);
        if (access(candidate.c_str(), R_OK) == 0) {
            LOG(INFO) << __func__ << ": using " << candidate;
            return candidate;
        }
        LOG(INFO) << __func__ << ": " << candidate << " is not readable";
    } else {
        LOG(INFO) << __func__ << ": not running from an APEX";
    }
    const std::string fallback =
            ::android::audio_find_readable_configuration_file(kEngineConfigFileName);
    if (fallback.empty()) {
        LOG(WARNING) << __func__ << ": no " << kEngineConfigFileName
                     << " found anywhere, the framework will use its built-in defaults";
    } else {
        LOG(INFO) << __func__ << ": using " << fallback;
    }
    return fallback;
}

MainlineConfig::MainlineConfig() : engine_converter_(FindEngineConfigFile()) {
    if (engine_converter_.getStatus() != ::android::OK) {
        LOG(WARNING) << __func__ << ": engine configuration: " << engine_converter_.getError();
    }
}

ndk::ScopedAStatus MainlineConfig::getSurroundSoundConfig(SurroundSoundConfig* _aidl_return) {
    *_aidl_return = internal::AudioPolicyConfigXmlConverter::getDefaultSurroundSoundConfig();
    LOG(DEBUG) << __func__ << ": " << _aidl_return->toString();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus MainlineConfig::getEngineConfig(AudioHalEngineConfig* _aidl_return) {
    if (engine_converter_.getStatus() == ::android::OK) {
        *_aidl_return = engine_converter_.getAidlEngineConfig();
    } else {
        *_aidl_return = AudioHalEngineConfig{};
    }
    LOG(DEBUG) << __func__ << ": " << _aidl_return->productStrategies.size()
               << " strategies, default strategy " << _aidl_return->defaultProductStrategyId << ", "
               << _aidl_return->volumeGroups.size() << " volume groups";
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core::mainline
