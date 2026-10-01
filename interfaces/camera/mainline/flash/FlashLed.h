/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <android-base/result.h>

namespace aidl::android::hardware::camera::mainline {

// One flash LED, driven at torch (continuous) brightness. Abstract for the
// unit tests.
class FlashLed {
  public:
    virtual ~FlashLed() = default;

    virtual const std::string& Name() const = 0;
    // Brightness levels above off; at least 1.
    virtual int32_t MaxLevel() const = 0;
    // Sets the brightness, 0 turns the LED off.
    virtual ::android::base::Result<void> SetLevel(int32_t level) = 0;
};

// The directory LED class devices are listed in.
constexpr char kLedClassDir[] = "/sys/class/leds";

// An LED class device, `<leds_dir>/<name>`: `brightness` from 0 to
// `max_brightness`.
::android::base::Result<std::unique_ptr<FlashLed>> OpenSysfsFlashLed(
        const std::string& name, const std::string& leds_dir = kLedClassDir);

// A V4L2 flash sub-device (/dev/v4l-subdevN): torch mode, with
// V4L2_CID_FLASH_TORCH_INTENSITY steps as levels when the device has it. The node
// stays open, which keeps the LED's sysfs interface disabled.
::android::base::Result<std::unique_ptr<FlashLed>> OpenV4l2FlashLed(const std::string& devnode);

// Opens a flash LED by the name a CameraCandidate uses for it: a device node
// ("/dev/...") is a V4L2 flash sub-device, anything else an LED class device.
::android::base::Result<std::unique_ptr<FlashLed>> OpenFlashLed(
        const std::string& name, const std::string& leds_dir = kLedClassDir);

// Whether the name of an LED class device ("<color>:<function>", or a
// legacy name such as "led:torch_0" or "flashlight") says that it is a
// camera flash or torch LED.
bool IsFlashLedName(const std::string& name);

// The camera flash LEDs among the LED class devices, sorted by name.
std::vector<std::string> ListFlashLeds(const std::string& leds_dir = kLedClassDir);

}  // namespace aidl::android::hardware::camera::mainline
