/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <vector>

#include "alsa/AlsaCard.h"
#include "passthrough/HdmiControl.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// How to find the ELD / "IEC958 Playback Default" control of an HDMI head.
enum class ControlLocator {
    // Interface PCM, device = the head's PCM device number (HDA ELD, ASoC
    // hdmi-codec on a plain link, Intel LPE).
    kPcmDevice,
    // Interface MIXER, index = position of the head among the card's HDMI
    // PCMs by device number (HDA IEC958 controls).
    kHdaOrdinal,
    // The only control of that name on the card, whatever its interface,
    // device and index (hdmi-codec behind a DPCM back-end, whose controls sit
    // on an internal PCM numbered after the link).
    kSoleControl,
};

const char* ToString(ControlLocator locator);

// Driver knowledge. The only place besides HdmiControl implementations that
// may look at driver names.
struct Quirk {
    const char* name;
    std::vector<ControlLocator> locators;
    HbrSupport hbr;
};

Quirk QuirkFor(const alsa::CardInfo& card);

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
