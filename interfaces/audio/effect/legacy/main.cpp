/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Main"

#include <cstdlib>
#include <string>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <system/audio_config.h>

#include "LegacyFactory.h"

using aidl::android::hardware::audio::effect::legacy::LegacyFactory;

namespace {

// Explicit configuration file, for devices that keep the legacy file
// somewhere unusual.
constexpr const char* kConfigProperty = "vendor.audio.effect_legacy.config";
constexpr const char* kVerboseProperty = "vendor.audio.effect_legacy.log.verbose";

// Same lookup as the legacy EffectsFactory: audio_effects.xml in /odm/etc,
// /vendor/etc, /system/etc. The AIDL example's file name is accepted too.
std::string FindConfigFile() {
    if (const std::string path = ::android::base::GetProperty(kConfigProperty, ""); !path.empty()) {
        LOG(INFO) << "using configuration from " << kConfigProperty << ": " << path;
        return path;
    }
    for (const char* name : {"audio_effects.xml", "audio_effects_config.xml"}) {
        const std::string path = ::android::audio_find_readable_configuration_file(name);
        if (!path.empty()) {
            LOG(INFO) << "using configuration " << path;
            return path;
        }
    }
    LOG(ERROR) << "no audio_effects.xml found, no effects will be available";
    return "";
}

}  // namespace

int main() {
    ::android::base::SetMinimumLogSeverity(::android::base::GetBoolProperty(kVerboseProperty, false)
                                                   ? ::android::base::VERBOSE
                                                   : ::android::base::DEBUG);
    LOG(INFO) << "Legacy audio effect HAL starting";

    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();

    auto factory = ndk::SharedRefBase::make<LegacyFactory>(FindConfigFile());
    const std::string fqn = std::string(LegacyFactory::descriptor) + "/default";
    if (const binder_status_t status =
                AServiceManager_addService(factory->asBinder().get(), fqn.c_str());
        status != STATUS_OK) {
        LOG(ERROR) << "failed to register " << fqn << ": " << status;
        return EXIT_FAILURE;
    }
    LOG(INFO) << "registered " << fqn << " with " << factory->effect_count() << " effect(s)";

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // Not reached.
}
