/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <alsa/use-case.h>

namespace aidl::android::hardware::audio::core::mainline::ucm {

// One UCM device of the selected verb together with the values the HAL
// cares about.
struct UcmDevice {
    std::string name;     // e.g. "Speaker", "Headphones", "Mic", "HDMI1"
    std::string comment;  // Human readable description from the profile.
    // Values with the direction prefix resolved: only one of the two PCM
    // names is set for a normal device (a few profiles set both).
    std::string playback_pcm;   // "PlaybackPCM", e.g. "hw:PCH,0"
    std::string capture_pcm;    // "CapturePCM"
    int playback_channels = 0;  // "PlaybackChannels", 0 when unspecified
    int capture_channels = 0;   // "CaptureChannels"
    int playback_priority = 0;  // "PlaybackPriority", higher is preferred
    int capture_priority = 0;   // "CapturePriority"
    std::string jack_control;   // "JackControl", name of the jack kcontrol
    std::vector<std::string> conflicting_devices;
    std::vector<std::string> supported_devices;

    bool HasPlayback() const { return !playback_pcm.empty(); }
    bool HasCapture() const { return !capture_pcm.empty(); }
    std::string ToString() const;
};

// Wrapper around snd_use_case_mgr_t for one sound card. Thread safe: every
// operation takes the internal lock. The manager stays open for the lifetime
// of the HAL so that the card keeps its routing state.
class UcmManager {
  public:
    // Opens the UCM profile of `card_index` ("hw:<index>"), runs the boot
    // sequences and selects `preferred_verb` (falling back to the first verb
    // of the profile). Returns nullptr when no profile exists or it fails to
    // load.
    static std::unique_ptr<UcmManager> Open(int card_index, const std::string& preferred_verb);
    ~UcmManager();

    UcmManager(const UcmManager&) = delete;
    UcmManager& operator=(const UcmManager&) = delete;

    int card_index() const { return card_index_; }
    const std::string& verb() const { return verb_; }
    const std::vector<UcmDevice>& devices() const { return devices_; }
    const UcmDevice* FindDevice(const std::string& name) const;

    // Enables a device, first disabling every enabled device that conflicts
    // with it. Returns 0 or a negative errno.
    int EnableDevice(const std::string& name);
    int DisableDevice(const std::string& name);
    bool IsDeviceEnabled(const std::string& name);

    // Raw value lookup, e.g. GetValue("PlaybackMixerElem/Speaker").
    std::optional<std::string> GetValue(const std::string& identifier);

  private:
    UcmManager(snd_use_case_mgr_t* mgr, int card_index);

    bool SelectVerb(const std::string& preferred_verb);
    void LoadDevices();
    std::vector<std::string> GetList(const std::string& identifier);
    std::vector<std::pair<std::string, std::string>> GetPairList(const std::string& identifier);
    std::optional<std::string> GetValueLocked(const std::string& identifier);
    bool IsDeviceEnabledLocked(const std::string& name);
    int SetLocked(const char* identifier, const char* value);

    std::mutex lock_;
    snd_use_case_mgr_t* mgr_;
    const int card_index_;
    std::string verb_;
    std::vector<UcmDevice> devices_;
};

}  // namespace aidl::android::hardware::audio::core::mainline::ucm
