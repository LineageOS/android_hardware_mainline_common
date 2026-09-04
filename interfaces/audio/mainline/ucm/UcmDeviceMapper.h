/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

#include "routing/DeviceRole.h"

namespace aidl::android::hardware::audio::core::mainline::ucm {

// Maps the name of a UCM device (SectionDevice."...") to a DeviceRole.
// alsa-ucm-conf uses a fairly stable vocabulary: "Speaker", "Headphones",
// "Headset", "Earpiece", "Handset", "Mic", "Headset Mic", "Internal Mic",
// "Line", "Line Out", "Line In", "HDMI1", "SPDIF", ... with optional numeric
// suffixes. `playback` selects the direction when the name alone is
// ambiguous ("Line", "Headset").
routing::DeviceRole ClassifyUcmDevice(const std::string& name, bool playback);

}  // namespace aidl::android::hardware::audio::core::mainline::ucm
