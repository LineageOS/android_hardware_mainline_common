/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "drmfb-composer3"

#include "Composer.h"

#include <android-base/properties.h>
#include <android/binder_ibinder_platform.h>
#include <log/log.h>
#include <unistd.h>

#include "ComposerClient.h"

namespace aidl::android::hardware::graphics::composer3::impl {

ndk::ScopedAStatus Composer::createClient(std::shared_ptr<IComposerClient>* out_client) {
    std::lock_guard lock(mutex_);
    if (!client_.expired()) {
        return ndk::ScopedAStatus::fromServiceSpecificError(IComposer::EX_NO_RESOURCES);
    }
    const std::string path = android::base::GetProperty("vendor.hwc.drm.device", "/dev/dri/card0");
    auto client = ndk::SharedRefBase::make<ComposerClient>(path);
    if (client == nullptr || !client->Init()) {
        ALOGE("Unable to initialize Composer client for %s", path.c_str());
        return ndk::ScopedAStatus::fromServiceSpecificError(IComposer::EX_NO_RESOURCES);
    }
    client_ = client;
    *out_client = std::move(client);
    ALOGI("Created singleton Composer client using %s", path.c_str());
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Composer::getCapabilities(std::vector<Capability>* capabilities) {
    capabilities->clear();
    return ndk::ScopedAStatus::ok();
}

binder_status_t Composer::dump(int fd, const char**, uint32_t) {
    std::shared_ptr<IComposerClient> client;
    {
        std::lock_guard lock(mutex_);
        client = client_.lock();
    }
    std::string output = "drmfb Composer3 service (no active client)\n";
    if (client != nullptr) {
        auto* implementation = static_cast<ComposerClient*>(client.get());
        output = implementation->Dump();
    }
    if (write(fd, output.data(), output.size()) < 0) return STATUS_BAD_VALUE;
    return STATUS_OK;
}

ndk::SpAIBinder Composer::createBinder() {
    auto binder = BnComposer::createBinder();
    AIBinder_setInheritRt(binder.get(), true);
    return binder;
}

}  // namespace aidl::android::hardware::graphics::composer3::impl
