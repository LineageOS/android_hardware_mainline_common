/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensors"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "Sensors.h"

using ::aidl::android::hardware::sensors::mainline::Sensors;

int main(int /* argc */, char** argv) {
    ::android::base::InitLogging(argv);
    LOG(INFO) << "Mainline Sensors HAL service starting";

    ABinderProcess_setThreadPoolMaxThreadCount(0);

    std::shared_ptr<Sensors> sensors = ndk::SharedRefBase::make<Sensors>();
    // Discover the hardware before the service becomes visible so that the
    // first getSensorsList() is complete and stable.
    sensors->Initialize();

    const std::string instance = std::string(Sensors::descriptor) + "/default";
    binder_status_t status =
            AServiceManager_addService(sensors->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << instance;
    LOG(INFO) << "Registered " << instance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // Not reached.
}
