/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "flash/FlashLed.h"

namespace aidl::android::hardware::camera::mainline {

// A FlashLed recording its level in an int the test owns (-1: never set).
class FakeFlashLed : public FlashLed {
  public:
    FakeFlashLed(std::string name, int32_t max_level, std::shared_ptr<std::atomic<int32_t>> level)
        : name_(std::move(name)), max_level_(max_level), level_(std::move(level)) {}

    const std::string& Name() const override { return name_; }
    int32_t MaxLevel() const override { return max_level_; }
    ::android::base::Result<void> SetLevel(int32_t level) override {
        *level_ = level;
        return {};
    }

  private:
    const std::string name_;
    const int32_t max_level_;
    const std::shared_ptr<std::atomic<int32_t>> level_;
};

}  // namespace aidl::android::hardware::camera::mainline
