/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "fb-allocator"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>
#include <sched.h>

#include <cstdlib>
#include <string>

#include "Allocator.h"

using aidl::android::hardware::graphics::allocator::IAllocator;
using aidl::android::hardware::graphics::allocator::impl::Allocator;

int main() {
    sched_param scheduler{.sched_priority = 2};
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &scheduler) != 0) {
        ALOGW("Unable to select real-time service scheduling");
    }
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    auto allocator = ndk::SharedRefBase::make<Allocator>();
    const std::string instance = std::string(IAllocator::descriptor) + "/default";
    if (allocator == nullptr ||
        AServiceManager_addServiceWithFlags(
                allocator->asBinder().get(), instance.c_str(),
                AServiceManager_AddServiceFlag::ADD_SERVICE_ALLOW_ISOLATED) != STATUS_OK) {
        ALOGE("Failed to register %s", instance.c_str());
        return EXIT_FAILURE;
    }
    ALOGI("Registered %s with mapper.fb", instance.c_str());
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
