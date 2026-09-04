/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aidl::android::hardware::audio::core::mainline::alsa {

// One PCM device (playback and/or capture) of a sound card, as reported by
// the control interface (snd_ctl_pcm_info).
struct PcmDeviceInfo {
    int card = -1;
    int device = -1;
    std::string id;    // e.g. "ALC256 Analog", "HDMI 0"
    std::string name;  // e.g. "ALC256 Analog", "HDMI 0"
    bool playback = false;
    bool capture = false;

    // "hw:<card>,<device>"
    std::string HwName() const;
    // Heuristics on the device name, used when no UCM profile is available.
    bool LooksLikeHdmi() const;
    bool LooksLikeSpdif() const;
    bool LooksLikeAnalog() const;
    std::string ToString() const;
};

// A sound card as reported by snd_ctl_card_info plus its PCM devices.
struct CardInfo {
    int index = -1;
    std::string id;         // e.g. "PCH"
    std::string driver;     // e.g. "HDA-Intel", "USB-Audio", "sof-hda-dsp"
    std::string name;       // e.g. "HDA Intel PCH"
    std::string long_name;  // e.g. "HDA Intel PCH at 0x... irq 130"
    std::string mixer_name;
    std::string components;
    std::vector<PcmDeviceInfo> pcms;

    // "hw:<index>", the name to use with snd_ctl_open() / snd_mixer_attach().
    std::string CtlName() const;
    bool IsUsb() const;
    bool HasPlayback() const;
    bool HasCapture() const;
    // True when at least one playback device is neither HDMI nor S/PDIF.
    bool HasAnalogPlayback() const;
    // Matches a user supplied selector: card index, card id or card name
    // (case insensitive).
    bool Matches(const std::string& selector) const;
    std::string ToString() const;
};

// Enumerates every sound card known to the kernel. Cards that can not be
// opened are skipped with a log message.
std::vector<CardInfo> EnumerateCards();

// Queries a single card by index.
std::optional<CardInfo> QueryCard(int index);

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
