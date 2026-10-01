/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Flash"

#include "flash/Flash.h"

#include <algorithm>

#include <android-base/logging.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::common::TorchModeStatus;

int32_t MaxLevel(const std::vector<std::unique_ptr<FlashLed>>& leds) {
    int32_t max = 1;
    for (const auto& led : leds) max = std::max(max, led->MaxLevel());
    return max;
}

}  // namespace

std::shared_ptr<Flash> Flash::Open(const std::vector<std::string>& names,
                                   const std::string& leds_dir) {
    std::vector<std::unique_ptr<FlashLed>> leds;
    for (const auto& name : names) {
        auto led = OpenFlashLed(name, leds_dir);
        if (!led.ok()) {
            LOG(WARNING) << "flash LED " << name << ": " << led.error().message();
            continue;
        }
        LOG(INFO) << "flash LED " << name << ": " << (*led)->MaxLevel() << " level(s)";
        leds.push_back(std::move(*led));
    }
    if (leds.empty()) return nullptr;
    return std::make_shared<Flash>(std::move(leds));
}

Flash::Flash(std::vector<std::unique_ptr<FlashLed>> leds)
    : leds_(std::move(leds)), max_level_(MaxLevel(leds_)), torch_level_(max_level_) {}

Flash::~Flash() {
    std::lock_guard<std::mutex> lock(lock_);
    ApplyLocked(0);
}

void Flash::SetTorchListener(TorchListener listener) {
    std::lock_guard<std::mutex> lock(lock_);
    listener_ = std::move(listener);
}

Status Flash::SetTorch(bool on) {
    std::lock_guard<std::mutex> lock(lock_);
    if (acquired_) return Status::CAMERA_IN_USE;
    torch_on_ = on;
    torch_level_ = default_level();
    ApplyLocked(on ? torch_level_ : 0);
    NotifyLocked(on ? TorchModeStatus::AVAILABLE_ON : TorchModeStatus::AVAILABLE_OFF);
    return Status::OK;
}

Status Flash::SetTorchLevel(int32_t level) {
    if (level < 1 || level > max_level_) return Status::ILLEGAL_ARGUMENT;
    std::lock_guard<std::mutex> lock(lock_);
    if (acquired_) return Status::CAMERA_IN_USE;
    const bool was_on = torch_on_;
    torch_on_ = true;
    torch_level_ = level;
    ApplyLocked(level);
    if (!was_on) NotifyLocked(TorchModeStatus::AVAILABLE_ON);
    return Status::OK;
}

int32_t Flash::torch_level() {
    std::lock_guard<std::mutex> lock(lock_);
    return torch_level_;
}

void Flash::Acquire() {
    std::lock_guard<std::mutex> lock(lock_);
    if (acquired_) return;
    acquired_ = true;
    torch_on_ = false;
    torch_level_ = default_level();
    ApplyLocked(0);
    NotifyLocked(TorchModeStatus::NOT_AVAILABLE);
}

void Flash::Release() {
    std::lock_guard<std::mutex> lock(lock_);
    if (!acquired_) return;
    acquired_ = false;
    ApplyLocked(0);
    NotifyLocked(TorchModeStatus::AVAILABLE_OFF);
}

void Flash::SetLit(bool lit) {
    std::lock_guard<std::mutex> lock(lock_);
    if (!acquired_) return;
    ApplyLocked(lit ? max_level_ : 0);
}

void Flash::Detach() {
    std::lock_guard<std::mutex> lock(lock_);
    listener_ = nullptr;
    acquired_ = false;
    torch_on_ = false;
    ApplyLocked(0);
}

void Flash::ApplyLocked(int32_t level) {
    for (const auto& led : leds_) {
        // Scale to the LED's own levels, rounding up so that it stays on.
        const int32_t led_level =
                level <= 0 ? 0
                           : static_cast<int32_t>((static_cast<int64_t>(level) * led->MaxLevel() +
                                                   max_level_ - 1) /
                                                  max_level_);
        if (auto result = led->SetLevel(led_level); !result.ok()) {
            LOG(ERROR) << "flash LED " << led->Name() << ": " << result.error().message();
        }
    }
}

void Flash::NotifyLocked(TorchModeStatus status) {
    if (listener_) listener_(status);
}

}  // namespace aidl::android::hardware::camera::mainline
