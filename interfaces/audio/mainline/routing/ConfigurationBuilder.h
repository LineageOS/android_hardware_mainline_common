/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>

#include <core-impl/Module.h>

#include "Properties.h"
#include "routing/DeviceInventory.h"

namespace aidl::android::hardware::audio::core::mainline::routing {

// Names of the mix ports created by the builder.
inline constexpr const char* kPrimaryOutputMixPort = "primary output";
inline constexpr const char* kMultichannelOutputMixPort = "multichannel output";
inline constexpr const char* kPrimaryInputMixPort = "primary input";
inline constexpr const char* kUsbOutputMixPort = "usb output";
inline constexpr const char* kUsbInputMixPort = "usb input";

// Builds the Module::Configuration (device ports, mix ports, routes and
// initial port configs) from the discovered endpoints and assigns the device
// port ids back into the inventory.
//
// Layout:
//   * one device port per endpoint;
//   * four USB device port templates (device / headset, in / out) served
//     through connectExternalDevice();
//   * "primary output" (PRIMARY flag) routed to every output device port;
//   * "multichannel output" (DIRECT flag) routed to the outputs that accept
//     six or more channels, only when such outputs exist;
//   * "primary input" routed from every input device port;
//   * "usb output" / "usb input" with dynamic profiles, routed to the USB
//     templates only.
std::unique_ptr<Module::Configuration> BuildConfiguration(DeviceInventory& inventory,
                                                          const Properties& properties);

}  // namespace aidl::android::hardware::audio::core::mainline::routing
