/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Routing"

#include "routing/RoutingController.h"

#include <sstream>

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core::mainline::routing {

RoutingController::RoutingController(std::shared_ptr<DeviceInventory> inventory)
    : inventory_(std::move(inventory)) {}

std::string RoutingController::Key(const Endpoint& endpoint) {
    return std::to_string(endpoint.card) + "/" + endpoint.ucm_device;
}

void RoutingController::Acquire(const Endpoint& endpoint) {
    if (endpoint.ucm_device.empty()) return;
    std::lock_guard guard(lock_);
    const int count = ++refcounts_[Key(endpoint)];
    LOG(DEBUG) << __func__ << ": \"" << endpoint.ucm_device << "\" on card " << endpoint.card
               << " refcount " << count;
    if (count != 1) return;
    ucm::UcmManager* ucm = inventory_->UcmForCard(endpoint.card);
    if (ucm == nullptr) {
        LOG(WARNING) << __func__ << ": no UCM manager for card " << endpoint.card;
        return;
    }
    if (const int err = ucm->EnableDevice(endpoint.ucm_device); err < 0) {
        LOG(ERROR) << __func__ << ": enabling UCM device \"" << endpoint.ucm_device << "\" failed ("
                   << err << "), audio may be inaudible";
    } else {
        LOG(INFO) << __func__ << ": enabled UCM device \"" << endpoint.ucm_device << "\" on card "
                  << endpoint.card;
    }
}

void RoutingController::Release(const Endpoint& endpoint) {
    if (endpoint.ucm_device.empty()) return;
    std::lock_guard guard(lock_);
    auto it = refcounts_.find(Key(endpoint));
    if (it == refcounts_.end() || it->second <= 0) {
        LOG(WARNING) << __func__ << ": \"" << endpoint.ucm_device << "\" was not acquired";
        return;
    }
    const int count = --it->second;
    LOG(DEBUG) << __func__ << ": \"" << endpoint.ucm_device << "\" on card " << endpoint.card
               << " refcount " << count;
    if (count != 0) return;
    refcounts_.erase(it);
    ucm::UcmManager* ucm = inventory_->UcmForCard(endpoint.card);
    if (ucm == nullptr) return;
    if (const int err = ucm->DisableDevice(endpoint.ucm_device); err < 0) {
        LOG(WARNING) << __func__ << ": disabling UCM device \"" << endpoint.ucm_device
                     << "\" failed (" << err << ")";
    } else {
        LOG(INFO) << __func__ << ": disabled UCM device \"" << endpoint.ucm_device << "\" on card "
                  << endpoint.card;
    }
}

std::string RoutingController::Dump() const {
    std::lock_guard guard(lock_);
    std::ostringstream os;
    os << "RoutingController: " << refcounts_.size() << " active UCM device(s)\n";
    for (const auto& [key, count] : refcounts_) {
        os << "  " << key << " refcount=" << count << "\n";
    }
    return os.str();
}

}  // namespace aidl::android::hardware::audio::core::mainline::routing
