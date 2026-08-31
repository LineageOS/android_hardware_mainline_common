/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-composer3"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>
#include <sched.h>

#include <cstdlib>
#include <string>

#include "Composer.h"

using aidl::android::hardware::graphics::composer3::IComposer;
using aidl::android::hardware::graphics::composer3::impl::Composer;

int main() {
    sched_param scheduler{.sched_priority = 2};
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &scheduler) != 0) {
        ALOGW("Unable to select real-time service scheduling");
    }
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    auto composer = ndk::SharedRefBase::make<Composer>();
    const std::string instance = std::string(IComposer::descriptor) + "/default";
    if (composer == nullptr ||
        AServiceManager_addServiceWithFlags(
                composer->asBinder().get(), instance.c_str(),
                AServiceManager_AddServiceFlag::ADD_SERVICE_ALLOW_ISOLATED) != STATUS_OK) {
        ALOGE("Failed to register %s", instance.c_str());
        return EXIT_FAILURE;
    }
    ALOGI("Registered %s", instance.c_str());
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
