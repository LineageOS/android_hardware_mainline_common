/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Sensors.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::sensors::mainline::Sensors;

int main() {
    LOG(INFO) << "Mainline Sensors HAL service starting";

    ABinderProcess_setThreadPoolMaxThreadCount(0);

    auto sensors = ndk::SharedRefBase::make<Sensors>();
    const std::string instance = std::string() + Sensors::descriptor + "/default";
    binder_status_t status =
            AServiceManager_addService(sensors->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register sensors service";

    LOG(INFO) << "Mainline Sensors HAL service registered as " << instance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
