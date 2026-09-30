/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <optional>

#include <aidl/android/hardware/camera/device/RequestTemplate.h>

#include "device/CameraDescription.h"
#include "utils/Metadata.h"

namespace aidl::android::hardware::camera::mainline {

// The default AE target fps range: a fixed one (for video) or a variable one,
// both with the highest frame rate up to 30 fps (or the lowest one if all are
// higher).
std::array<int32_t, 2> DefaultFpsRange(const CameraDescription& description, bool fixed);

// Default settings of a request template, or nullopt for templates the camera
// does not support (ZERO_SHUTTER_LAG, MANUAL).
std::optional<Metadata> BuildRequestTemplate(const CameraDescription& description,
                                             device::RequestTemplate type);

}  // namespace aidl::android::hardware::camera::mainline
