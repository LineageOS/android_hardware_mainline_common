/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <aidl/android/media/audio/common/AudioDevice.h>
#include <aidl/android/media/audio/common/AudioProfile.h>

#include "alsa/AlsaFormat.h"
#include "routing/DeviceRole.h"

namespace aidl::android::hardware::audio::core::mainline::routing {

// An audio endpoint: the thing behind one device port. It binds the Android
// view (AudioDevice, role, profiles) to the ALSA view (card, PCM device name,
// optional UCM device) of one playback or capture path.
struct Endpoint {
    enum class Backend {
        kAlsa,  // A real PCM device.
        kNull,  // Placeholder used when no hardware exists: discards / zeroes.
    };

    // --- Android side ---
    std::string name;  // AudioPort.name, e.g. "Speaker", "PCH: HDMI2"
    DeviceRole role = DeviceRole::kBusOut;
    bool is_input = false;
    bool is_default = false;  // Carries FLAG_INDEX_DEFAULT_DEVICE.
    ::aidl::android::media::audio::common::AudioDevice device;
    std::vector<::aidl::android::media::audio::common::AudioProfile> profiles;
    int32_t port_id = 0;  // Assigned when the module configuration is built.

    // --- ALSA side ---
    Backend backend = Backend::kAlsa;
    int card = -1;
    std::string card_id;              // e.g. "PCH"
    std::string pcm_name;             // e.g. "hw:0,0"
    std::string ucm_device;           // UCM device to enable, empty without UCM.
    int priority = 0;                 // UCM priority, higher wins.
    unsigned int fixed_channels = 0;  // UCM Playback/CaptureChannels, 0 = any.
    alsa::HwCapabilities caps;

    bool IsAttached() const { return !IsExternalRole(role); }
    bool IsNull() const { return backend == Backend::kNull; }
    std::string ToString() const;
};

}  // namespace aidl::android::hardware::audio::core::mainline::routing
