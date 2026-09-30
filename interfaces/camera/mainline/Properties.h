/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aidl::android::hardware::camera::mainline {

// All Android properties understood by the HAL. Every key carries the
// "vendor.camera." prefix. Values are read once at start-up (per-device ones
// when the device is discovered); the HAL has to be restarted for a change
// to take effect.
//
// The names are documented in README.md. Keep both in sync.
struct Properties {
    // Treat cameras as internal (listed at boot, front / back facing) unless
    // something says otherwise. False: treat them as external.
    bool default_internal = false;

    // Number of internal cameras to wait for before the provider is
    // registered, and the maximum time to wait for them.
    int wait_internal_count = 0;
    int wait_internal_ms = 10000;

    // First camera ID given to external cameras. Internal cameras use the IDs
    // below it.
    int external_id_offset = 100;

    // Log verbosely (sets the minimum severity to VERBOSE instead of DEBUG).
    bool verbose_logging = false;

    // Per-device properties, "vendor.camera.device.<selector>.<key>". A
    // device matches several selectors (see DeviceSelectors()); for every key
    // the most specific selector that sets it wins.
    struct DeviceProperties {
        // False: ignore the device.
        std::optional<bool> enabled;
        // Internal or external camera, overriding every detection.
        std::optional<bool> internal;
    };

    static Properties Load();
    std::string ToString() const;

    // Reads the per-device properties of a device matching `selectors`, most
    // specific first. `get` returns the value of a property, or an empty
    // string when it is unset; it is a parameter for the unit tests.
    static DeviceProperties LoadDeviceProperties(
            const std::vector<std::string>& selectors,
            const std::function<std::string(const std::string&)>& get);
    static DeviceProperties LoadDeviceProperties(const std::vector<std::string>& selectors);

    // Turns an arbitrary string (a card name, a bus_info) into something
    // usable inside a property name: every character other than [0-9A-Za-z]
    // and ":@-" becomes '_'. Dots are replaced as well, as they separate the
    // selector from the key.
    static std::string SanitizeSelector(const std::string& value);

    static constexpr const char* kPrefix = "vendor.camera.";
};

}  // namespace aidl::android::hardware::camera::mainline
