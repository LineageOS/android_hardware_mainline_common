/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <optional>

#include <aidl/android/hardware/camera/device/RequestTemplate.h>

#include "device/CameraDescription.h"
#include "utils/Metadata.h"

namespace aidl::android::hardware::camera::mainline {

// Default settings of a request template, or nullopt for templates the camera
// does not support (ZERO_SHUTTER_LAG, MANUAL).
std::optional<Metadata> BuildRequestTemplate(const CameraDescription& description,
                                             device::RequestTemplate type);

}  // namespace aidl::android::hardware::camera::mainline
