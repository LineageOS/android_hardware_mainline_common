/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "passthrough/Quirks.h"

#include <string>

namespace aidl::android::hardware::audio::core::mainline::passthrough {

namespace {

// HDA: the ELD is a PCM control of the HDMI PCM device, the IEC958 controls
// are mixer controls numbered by HDMI PCM (sound/hda/common/codec.c,
// snd_hda_create_dig_out_ctls). The non-audio bit of the channel status
// selects the non-PCM stream format at prepare time, and high bit rate is
// refused there with -EINVAL when the pin (or, on Tegra 20 / 30, the display
// side) can not do it, which the runtime fallback handles. The HDA codec
// layer adds "HDA:<codec ids>" to the card components, whatever the
// controller driver.
bool IsHda(const alsa::CardInfo& card) {
    return card.driver == "HDA-Intel" || card.driver == "tegra-hda" ||
           card.components.find("HDA:") != std::string::npos;
}

}  // namespace

const char* ToString(ControlLocator locator) {
    switch (locator) {
        case ControlLocator::kPcmDevice:
            return "pcm-device";
        case ControlLocator::kHdaOrdinal:
            return "hda-ordinal";
        case ControlLocator::kSoleControl:
            return "sole-control";
    }
    return "?";
}

Quirk QuirkFor(const alsa::CardInfo& card) {
    if (IsHda(card)) {
        return Quirk{.name = "hda",
                     .locators = {ControlLocator::kPcmDevice, ControlLocator::kHdaOrdinal},
                     .hbr = HbrSupport::kSupported};
    }
    // Anything else, typically ASoC hdmi-codec (e.g. Amlogic dw-hdmi through
    // an I2S bridge that knows nothing about high bit rate): no HDA style
    // numbering, and an HBR attempt may seem to work while sending garbage.
    return Quirk{.name = "default",
                 .locators = {ControlLocator::kPcmDevice, ControlLocator::kSoleControl},
                 .hbr = HbrSupport::kUndetectable};
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
