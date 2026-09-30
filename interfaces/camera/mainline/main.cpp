/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Main"

#include <chrono>
#include <cstdlib>
#include <string>

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "Properties.h"
#include "provider/CameraProvider.h"

using aidl::android::hardware::camera::mainline::CameraProvider;
using aidl::android::hardware::camera::mainline::Properties;

namespace {

// Recommended binder thread count for camera providers (see the AOSP
// reference implementation).
constexpr int kBinderThreadPoolSize = 6;

constexpr char kInstance[] = "internal/0";

}  // namespace

int main() {
    const Properties properties = Properties::Load();
    ::android::base::SetMinimumLogSeverity(properties.verbose_logging ? ::android::base::VERBOSE
                                                                      : ::android::base::DEBUG);
    LOG(INFO) << "Mainline camera HAL starting";

    ABinderProcess_setThreadPoolMaxThreadCount(kBinderThreadPoolSize);
    ABinderProcess_startThreadPool();

    auto provider = ndk::SharedRefBase::make<CameraProvider>(properties);
    provider->Start();

    // The framework reads the list of internal cameras only once, when the
    // provider is registered. Internal cameras that show up later are still
    // announced, like external ones, but may then get an unexpected ID order.
    if (properties.wait_internal_count > 0) {
        provider->WaitForInternalCameras(properties.wait_internal_count,
                                         std::chrono::milliseconds(properties.wait_internal_ms));
    }

    const std::string name = std::string(CameraProvider::descriptor) + "/" + kInstance;
    if (const binder_status_t status =
                AServiceManager_addService(provider->asBinder().get(), name.c_str());
        status != STATUS_OK) {
        LOG(FATAL) << "failed to register " << name << ": " << status;
    }
    LOG(INFO) << "registered " << name;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // Not reached.
}
