/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

namespace aidl::android::hardware::audio::core::mainline::routing {

// The role an ALSA playback or capture path plays from Android's point of
// view. It determines the AudioDeviceType / connection of the device port that
// represents the path and whether the port is permanently attached or a
// template for an external device.
enum class DeviceRole {
    // Outputs
    kSpeaker,     // OUT_SPEAKER, attached, default output
    kEarpiece,    // OUT_SPEAKER_EARPIECE, attached
    kHeadphones,  // OUT_HEADPHONE / analog, external template
    kHeadset,     // OUT_HEADSET / analog, external template
    kLineOut,     // OUT_DEVICE / analog, external template
    kHdmi,        // OUT_DEVICE / hdmi, external template
    kSpdif,       // OUT_DEVICE / spdif, external template
    kBusOut,      // OUT_BUS, attached, addressed (any other output)
    // Inputs
    kMic,         // IN_MICROPHONE, attached, default input
    kHeadsetMic,  // IN_HEADSET / analog, external template
    kBusIn,       // IN_BUS, attached, addressed (line in, extra mics, ...)
};

inline bool IsInputRole(DeviceRole role) {
    return role == DeviceRole::kMic || role == DeviceRole::kHeadsetMic ||
           role == DeviceRole::kBusIn;
}

// Roles that end up as external (template) device ports.
inline bool IsExternalRole(DeviceRole role) {
    switch (role) {
        case DeviceRole::kHeadphones:
        case DeviceRole::kHeadset:
        case DeviceRole::kLineOut:
        case DeviceRole::kHdmi:
        case DeviceRole::kSpdif:
        case DeviceRole::kHeadsetMic:
            return true;
        default:
            return false;
    }
}

inline const char* ToString(DeviceRole role) {
    switch (role) {
        case DeviceRole::kSpeaker:
            return "Speaker";
        case DeviceRole::kEarpiece:
            return "Earpiece";
        case DeviceRole::kHeadphones:
            return "Headphones";
        case DeviceRole::kHeadset:
            return "Headset";
        case DeviceRole::kLineOut:
            return "LineOut";
        case DeviceRole::kHdmi:
            return "HDMI";
        case DeviceRole::kSpdif:
            return "SPDIF";
        case DeviceRole::kBusOut:
            return "BusOut";
        case DeviceRole::kMic:
            return "Mic";
        case DeviceRole::kHeadsetMic:
            return "HeadsetMic";
        case DeviceRole::kBusIn:
            return "BusIn";
    }
    return "?";
}

}  // namespace aidl::android::hardware::audio::core::mainline::routing
