/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <set>
#include <string>
#include <vector>

namespace aidl::android::hardware::audio::core::mainline {

// All Android properties understood by the HAL. Every key carries the
// "vendor.audio.mainline." prefix. Values are read once at start-up; the HAL
// has to be restarted for a change to take effect.
//
// The names are documented in README.md. Keep both in sync.
struct Properties {
    // Comma separated list of sound cards to use. Each entry is either a card
    // index ("0"), a card id ("PCH") or a card name ("HDA Intel PCH"). An empty
    // list means "every card except USB ones".
    std::vector<std::string> cards;

    // Maximum time in milliseconds to wait for the cards listed in `cards` to
    // appear before proceeding with enumeration. 0 means no waiting (use
    // whatever is present). Only effective when `cards` is non-empty.
    int wait_for_cards_ms = 0;

    // Card providing the built-in "Speaker" and "Built-In Mic" device ports.
    // Same syntax as a single entry of `cards`. Empty means "pick the first
    // card that has an analog playback device".
    std::string primary_card;

    // Also expose USB sound cards that are present at start-up as static
    // cards. Normally USB cards are handled through the connectExternalDevice
    // flow driven by the framework's UsbAlsaManager.
    bool include_usb_cards = false;

    // Use the ALSA Use Case Manager (alsa-ucm-conf) to discover and route
    // devices when a profile exists for the card.
    bool ucm_enabled = true;

    // UCM verb to select. The first available verb is used when the requested
    // one does not exist.
    std::string ucm_verb = "HiFi";

    // For cards without a UCM profile: unmute the mixer and apply default
    // volumes at start-up.
    bool mixer_init = true;
    int mixer_playback_percent = 100;
    int mixer_capture_percent = 80;

    // Nominal latency reported for PCM streams, also drives the stream buffer
    // size negotiated with the framework.
    int latency_ms = 20;

    // Expose a DIRECT "multichannel output" mix port when a device supports
    // six or more channels.
    bool multichannel = true;

    // Log verbosely (sets the minimum severity to VERBOSE instead of DEBUG).
    bool verbose_logging = false;

    // Per-card properties: sample rates and bit depths to allow for each card.
    // Empty sets mean "use all rates/bits supported by the hardware".
    // Read at enumeration time using card id, index, or name (with spaces
    // replaced by underscores) as the selector.
    struct CardProperties {
        std::set<int> rates;
        std::set<int> bits;
    };

    static Properties Load();
    std::string ToString() const;

    // Reads per-card properties for a card, merging across all matching
    // selectors (id, index, name with spaces as underscores).
    static CardProperties LoadCardProperties(const std::string& card_id, int card_index,
                                             const std::string& card_name);

    static constexpr const char* kPrefix = "vendor.audio.mainline.";
};

}  // namespace aidl::android::hardware::audio::core::mainline
