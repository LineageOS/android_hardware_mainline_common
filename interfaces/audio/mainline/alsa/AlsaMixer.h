/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

namespace aidl::android::hardware::audio::core::mainline::alsa {

struct MixerInitOptions {
    // Percentage of the control range applied to playback volume controls.
    int playback_percent = 100;
    // Percentage of the control range applied to capture volume controls.
    int capture_percent = 80;
};

// Brings the simple mixer of a card into a usable state, in the spirit of
// "alsactl init": every playback switch is turned on, every playback volume
// is set to `playback_percent`, capture switches are turned on and capture
// volumes set to `capture_percent`. Controls that would be harmful when
// enabled blindly (loopbacks, beeps, mic boosts) are left alone.
//
// This is only used for cards that have no UCM profile; a UCM profile knows
// far better which controls matter. Returns false when the mixer could not be
// opened.
bool InitializeMixer(int card, const MixerInitOptions& options);

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
