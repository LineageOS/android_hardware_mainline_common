/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <aidl/android/hardware/graphics/composer3/BnComposer.h>

#include <mutex>

namespace aidl::android::hardware::graphics::composer3::impl {

class Composer : public BnComposer {
  public:
    ndk::ScopedAStatus createClient(std::shared_ptr<IComposerClient>* client) override;
    ndk::ScopedAStatus getCapabilities(std::vector<Capability>* capabilities) override;
    binder_status_t dump(int fd, const char** args, uint32_t num_args) override;

  protected:
    ndk::SpAIBinder createBinder() override;

  private:
    std::mutex mutex_;
    std::weak_ptr<IComposerClient> client_;
};

}  // namespace aidl::android::hardware::graphics::composer3::impl
