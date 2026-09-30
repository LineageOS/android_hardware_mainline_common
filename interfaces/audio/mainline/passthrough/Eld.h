/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "passthrough/SinkCapabilities.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// Parses the EDID-Like Data an HDMI / DisplayPort sink reports through the
// ELD control (HDA specification, CTA-861 short audio descriptors) into the
// formats it decodes. Returns nullopt for an empty ELD (no sink, or an ELD
// the driver does not consider valid) and for a malformed one.
std::optional<SinkCapabilities> ParseEld(const std::vector<uint8_t>& eld);

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
