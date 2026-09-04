/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_UcmMapper"

#include "ucm/UcmDeviceMapper.h"

#include <algorithm>
#include <cctype>

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core::mainline::ucm {

using routing::DeviceRole;

namespace {

std::string Normalize(const std::string& name) {
    // Lower case, no spaces / dashes / underscores: "Headset Mic" -> "headsetmic".
    std::string out;
    out.reserve(name.size());
    for (const unsigned char c : name) {
        if (c == ' ' || c == '-' || c == '_') continue;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

bool StartsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

}  // namespace

DeviceRole ClassifyUcmDevice(const std::string& name, bool playback) {
    const std::string n = Normalize(name);
    DeviceRole role;

    if (!playback) {
        if (Contains(n, "headsetmic") || Contains(n, "headphonemic") || Contains(n, "hsmic") ||
            Contains(n, "headset")) {
            role = DeviceRole::kHeadsetMic;
        } else if (Contains(n, "line") || Contains(n, "aux") || Contains(n, "spdif") ||
                   Contains(n, "iec958") || Contains(n, "hdmi") || Contains(n, "usb") ||
                   Contains(n, "monitor") || Contains(n, "loopback")) {
            role = DeviceRole::kBusIn;
        } else if (Contains(n, "mic") || Contains(n, "dmic") || Contains(n, "capture") ||
                   Contains(n, "handset")) {
            // "Mic", "Mic1", "Internal Mic", "Mainmic", "Handsetmic", "DMIC"...
            role = DeviceRole::kMic;
        } else {
            role = DeviceRole::kBusIn;
        }
    } else {
        if (Contains(n, "earpiece") || Contains(n, "handset") || Contains(n, "receiver")) {
            role = DeviceRole::kEarpiece;
        } else if (StartsWith(n, "speaker") || Contains(n, "spk")) {
            role = DeviceRole::kSpeaker;
        } else if (Contains(n, "headphone")) {
            role = DeviceRole::kHeadphones;
        } else if (Contains(n, "headset")) {
            role = DeviceRole::kHeadset;
        } else if (Contains(n, "hdmi") || Contains(n, "displayport") || StartsWith(n, "dp")) {
            role = DeviceRole::kHdmi;
        } else if (Contains(n, "spdif") || Contains(n, "iec958") || Contains(n, "digital") ||
                   Contains(n, "optical") || Contains(n, "toslink")) {
            role = DeviceRole::kSpdif;
        } else if (Contains(n, "lineout") || StartsWith(n, "line") || Contains(n, "aux")) {
            role = DeviceRole::kLineOut;
        } else {
            role = DeviceRole::kBusOut;
        }
    }
    LOG(DEBUG) << __func__ << ": \"" << name << "\" (" << (playback ? "playback" : "capture")
               << ") -> " << routing::ToString(role);
    return role;
}

}  // namespace aidl::android::hardware::audio::core::mainline::ucm
