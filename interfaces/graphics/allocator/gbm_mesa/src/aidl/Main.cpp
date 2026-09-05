/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#include "Allocator.h"

#define LOG_TAG "allocator-gm"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "log.h"

using aidl::android::hardware::graphics::allocator::impl::GbmMesaAllocator;

int main(int /*argc*/, char** /*argv*/) {
    log_i("GBM Mesa AIDL allocator starting up...");

    // same as SF main thread
    struct sched_param param = {0};
    param.sched_priority = 2;
    if (sched_setscheduler(0, SCHED_FIFO | SCHED_RESET_ON_FORK, &param) != 0) {
        log_i("%s: failed to set priority: %s", __FUNCTION__, strerror(errno));
    }

    auto allocator = ndk::SharedRefBase::make<GbmMesaAllocator>();
    CHECK(allocator != nullptr);

    if (!allocator->init()) {
        log_e("Failed to initialize GBM Mesa AIDL allocator.");
        return EXIT_FAILURE;
    }

    const std::string instance = std::string() + GbmMesaAllocator::descriptor + "/default";
    binder_status_t status =
            AServiceManager_addService(allocator->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK);

    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    ABinderProcess_joinThreadPool();

    return EXIT_FAILURE;
}