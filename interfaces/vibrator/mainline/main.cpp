/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineVibratorMain"

#include "vibrator-impl/Vibrator.h"
#include "vibrator-impl/VibratorManager.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::vibrator::Vibrator;
using aidl::android::hardware::vibrator::VibratorManager;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    auto vib = ndk::SharedRefBase::make<Vibrator>();
    if (!vib->init()) {
        LOG(FATAL) << "Vibrator: failed to initialize";
    }

    binder_status_t status = AServiceManager_addService(
            vib->asBinder().get(), Vibrator::makeServiceName("default").c_str());
    CHECK_EQ(status, STATUS_OK);

    auto managedVib = ndk::SharedRefBase::make<Vibrator>();
    if (!managedVib->init()) {
        LOG(FATAL) << "Vibrator: failed to initialize managed vibrator";
    }

    auto vibManager = ndk::SharedRefBase::make<VibratorManager>(std::move(managedVib));
    status = AServiceManager_addService(vibManager->asBinder().get(),
                                         VibratorManager::makeServiceName("default").c_str());
    CHECK_EQ(status, STATUS_OK);

    LOG(INFO) << "Vibrator: service started";
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
