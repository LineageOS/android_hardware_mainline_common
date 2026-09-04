/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "routing/DeviceInventory.h"
#include "routing/Endpoint.h"

namespace aidl::android::hardware::audio::core::mainline::routing {

// Keeps the hardware routing (UCM device enable / disable) in sync with the
// streams that use an endpoint. Multiple streams may share an endpoint, so
// the UCM device is enabled on the first Acquire() and disabled on the last
// Release(). Endpoints without a UCM device are no-ops here: their PCM device
// is simply opened by the stream.
class RoutingController {
  public:
    explicit RoutingController(std::shared_ptr<DeviceInventory> inventory);

    void Acquire(const Endpoint& endpoint);
    void Release(const Endpoint& endpoint);

    std::string Dump() const;

  private:
    static std::string Key(const Endpoint& endpoint);

    const std::shared_ptr<DeviceInventory> inventory_;
    mutable std::mutex lock_;
    std::map<std::string, int> refcounts_;
};

}  // namespace aidl::android::hardware::audio::core::mainline::routing
