/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Main"

#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include <Log.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <core-impl/ChildInterface.h>
#include <core-impl/Module.h>

#include "MainlineConfig.h"
#include "ModuleMainline.h"
#include "Properties.h"
#include "alsa/AlsaError.h"

using aidl::android::hardware::audio::core::ChildInterface;
using aidl::android::hardware::audio::core::Module;
using aidl::android::hardware::audio::core::mainline::MainlineConfig;
using aidl::android::hardware::audio::core::mainline::ModuleMainline;
using aidl::android::hardware::audio::core::mainline::Properties;

namespace {

constexpr int kBinderThreadPoolSize = 16;

// Registers `module` as android.hardware.audio.core.IModule/<instance>.
bool RegisterModule(const std::shared_ptr<Module>& module, const std::string& instance,
                    std::vector<ChildInterface<Module>>* keep_alive) {
    if (module == nullptr) {
        LOG(ERROR) << "no module instance for " << instance;
        return false;
    }
    ChildInterface<Module> child;
    child = module;
    const std::string fqn = std::string(Module::descriptor) + "/" + instance;
    if (const binder_status_t status = AServiceManager_addService(child.getBinder(), fqn.c_str());
        status != STATUS_OK) {
        LOG(ERROR) << "failed to register " << fqn << ": " << status;
        return false;
    }
    LOG(INFO) << "registered " << fqn;
    keep_alive->push_back(std::move(child));
    return true;
}

// Instantiates one of the example HAL's software modules (r_submix,
// bluetooth). These have no hardware dependency.
void RegisterExampleModule(Module::Type type, const std::string& instance,
                           std::vector<ChildInterface<Module>>* keep_alive) {
    std::shared_ptr<Module> module = Module::createInstance(type);
    if (module == nullptr) {
        LOG(ERROR) << "failed to create the " << instance << " module";
        return;
    }
    RegisterModule(module, instance, keep_alive);
}

}  // namespace

int main() {
    // The example HAL's stream implementation uses std::rand() for the
    // internal command cookie.
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    const Properties properties = Properties::Load();
    ::android::base::SetMinimumLogSeverity(properties.verbose_logging ? ::android::base::VERBOSE
                                                                      : ::android::base::DEBUG);
    LOG(INFO) << "Mainline audio HAL starting";

    ABinderProcess_setThreadPoolMaxThreadCount(kBinderThreadPoolSize);
    ABinderProcess_startThreadPool();

    aidl::android::hardware::audio::core::mainline::alsa::InstallAlsaErrorHandler();

    // IConfig/default
    auto config = ndk::SharedRefBase::make<MainlineConfig>();
    const std::string config_fqn = std::string(MainlineConfig::descriptor) + "/default";
    if (const binder_status_t status =
                AServiceManager_addService(config->asBinder().get(), config_fqn.c_str());
        status != STATUS_OK) {
        LOG(ERROR) << "failed to register " << config_fqn << ": " << status;
    } else {
        LOG(INFO) << "registered " << config_fqn;
    }

    // IModule/default: the alsa-lib backed module. Discovery happens here, so
    // the sound cards must already exist when the HAL starts.
    std::vector<ChildInterface<Module>> modules;
    RegisterModule(ModuleMainline::Create(properties), "default", &modules);

    // Software modules reused from the example HAL.
    RegisterExampleModule(Module::Type::R_SUBMIX, "r_submix", &modules);
#ifdef MAINLINE_AUDIO_WITH_BLUETOOTH
    RegisterExampleModule(Module::Type::BLUETOOTH, "bluetooth", &modules);
#endif

    LOG(INFO) << "Mainline audio HAL ready with " << modules.size() << " module(s)";
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // Not reached.
}
