/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Ucm"

#include "ucm/UcmManager.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "alsa/AlsaError.h"

namespace aidl::android::hardware::audio::core::mainline::ucm {

using alsa::ErrorString;

// --- UcmDevice ---------------------------------------------------------------

std::string UcmDevice::ToString() const {
    std::ostringstream os;
    os << "\"" << name << "\"";
    if (!comment.empty()) os << " (" << comment << ")";
    if (HasPlayback()) {
        os << " playback=" << playback_pcm << " ch=" << playback_channels
           << " prio=" << playback_priority;
    }
    if (HasCapture()) {
        os << " capture=" << capture_pcm << " ch=" << capture_channels
           << " prio=" << capture_priority;
    }
    if (!jack_control.empty()) os << " jack=\"" << jack_control << "\"";
    if (!conflicting_devices.empty()) {
        os << " conflicts=[" << ::android::base::Join(conflicting_devices, ",") << "]";
    }
    return os.str();
}

// --- UcmManager --------------------------------------------------------------

UcmManager::UcmManager(snd_use_case_mgr_t* mgr, int card_index)
    : mgr_(mgr), card_index_(card_index) {}

UcmManager::~UcmManager() {
    std::lock_guard guard(lock_);
    if (mgr_ != nullptr) {
        // Restore the card to its default state so that the next HAL instance
        // starts from a known configuration.
        if (const int err = snd_use_case_mgr_reset(mgr_); err < 0) {
            LOG(DEBUG) << "snd_use_case_mgr_reset(card " << card_index_
                       << "): " << ErrorString(err);
        }
        snd_use_case_mgr_close(mgr_);
        mgr_ = nullptr;
    }
}

std::unique_ptr<UcmManager> UcmManager::Open(int card_index, const std::string& preferred_verb) {
    const std::string card_name = "hw:" + std::to_string(card_index);
    snd_use_case_mgr_t* mgr = nullptr;
    if (const int err = snd_use_case_mgr_open(&mgr, card_name.c_str()); err < 0) {
        // -ENOENT / -ENXIO: no profile for this card. Perfectly normal.
        LOG(INFO) << __func__ << ": no usable UCM profile for " << card_name << ": "
                  << ErrorString(err);
        return nullptr;
    }
    std::unique_ptr<UcmManager> manager(new UcmManager(mgr, card_index));

    {
        std::lock_guard guard(manager->lock_);
        // The boot sequences are normally run by udev on a desktop system.
        // Nothing does that on Android, so run them here. Both are optional.
        for (const char* sequence : {"_fboot", "_boot"}) {
            const int err = snd_use_case_set(mgr, sequence, nullptr);
            if (err < 0 && err != -ENOENT) {
                LOG(WARNING) << __func__ << ": " << card_name << " " << sequence
                             << " failed: " << ErrorString(err);
            } else {
                LOG(DEBUG) << __func__ << ": " << card_name << " " << sequence << " done";
            }
        }
    }

    if (!manager->SelectVerb(preferred_verb)) {
        return nullptr;
    }
    manager->LoadDevices();

    std::string file;
    if (auto value = manager->GetValue("_file"); value.has_value()) file = *value;
    LOG(INFO) << __func__ << ": " << card_name << " profile \"" << file << "\" verb \""
              << manager->verb_ << "\" with " << manager->devices_.size() << " device(s)";
    for (const auto& device : manager->devices_) {
        LOG(INFO) << "  " << device.ToString();
    }
    return manager;
}

std::vector<std::string> UcmManager::GetList(const std::string& identifier) {
    // Caller holds lock_.
    std::vector<std::string> result;
    const char** list = nullptr;
    const int count = snd_use_case_get_list(mgr_, identifier.c_str(), &list);
    if (count < 0) {
        if (count != -ENOENT) {
            LOG(DEBUG) << __func__ << ": " << identifier << ": " << ErrorString(count);
        }
        return result;
    }
    for (int i = 0; i < count; ++i) {
        if (list[i] != nullptr) result.emplace_back(list[i]);
    }
    snd_use_case_free_list(list, count);
    return result;
}

std::vector<std::pair<std::string, std::string>> UcmManager::GetPairList(
        const std::string& identifier) {
    // Caller holds lock_. "_verbs" and "_devices[/verb]" return (name, comment)
    // pairs, see snd_use_case_get_list().
    std::vector<std::pair<std::string, std::string>> result;
    const char** list = nullptr;
    const int count = snd_use_case_get_list(mgr_, identifier.c_str(), &list);
    if (count < 0) {
        if (count != -ENOENT) {
            LOG(DEBUG) << __func__ << ": " << identifier << ": " << ErrorString(count);
        }
        return result;
    }
    for (int i = 0; i + 1 < count; i += 2) {
        if (list[i] != nullptr) {
            result.emplace_back(list[i], list[i + 1] != nullptr ? list[i + 1] : "");
        }
    }
    snd_use_case_free_list(list, count);
    return result;
}

std::optional<std::string> UcmManager::GetValueLocked(const std::string& identifier) {
    const char* value = nullptr;
    const int err = snd_use_case_get(mgr_, identifier.c_str(), &value);
    if (err < 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    free(const_cast<char*>(value));
    return result;
}

std::optional<std::string> UcmManager::GetValue(const std::string& identifier) {
    std::lock_guard guard(lock_);
    return GetValueLocked(identifier);
}

int UcmManager::SetLocked(const char* identifier, const char* value) {
    const int err = snd_use_case_set(mgr_, identifier, value);
    if (err < 0) {
        LOG(WARNING) << "card " << card_index_ << ": snd_use_case_set(" << identifier << ", "
                     << (value != nullptr ? value : "null") << "): " << ErrorString(err);
    } else {
        LOG(DEBUG) << "card " << card_index_ << ": " << identifier << " "
                   << (value != nullptr ? value : "");
    }
    return err;
}

bool UcmManager::SelectVerb(const std::string& preferred_verb) {
    std::lock_guard guard(lock_);
    std::vector<std::string> verbs;
    for (const auto& [name, comment] : GetPairList("_verbs")) verbs.push_back(name);
    if (verbs.empty()) {
        LOG(WARNING) << __func__ << ": card " << card_index_ << " profile has no verbs";
        return false;
    }
    std::string verb = verbs.front();
    if (std::find(verbs.begin(), verbs.end(), preferred_verb) != verbs.end()) {
        verb = preferred_verb;
    } else {
        LOG(INFO) << __func__ << ": card " << card_index_ << " has no verb \"" << preferred_verb
                  << "\", using \"" << verb
                  << "\" (available: " << ::android::base::Join(verbs, ", ") << ")";
    }
    if (SetLocked("_verb", verb.c_str()) < 0) {
        return false;
    }
    verb_ = verb;
    return true;
}

void UcmManager::LoadDevices() {
    std::lock_guard guard(lock_);
    devices_.clear();
    for (const auto& [name, comment] : GetPairList("_devices/" + verb_)) {
        UcmDevice device;
        device.name = name;
        device.comment = comment;
        const std::string suffix = "/" + name;
        auto get_string = [this, &suffix](const char* key) {
            return GetValueLocked(std::string(key) + suffix).value_or("");
        };
        auto get_int = [&get_string](const char* key) {
            int value = 0;
            const std::string text = get_string(key);
            if (!text.empty() && !::android::base::ParseInt(text, &value)) value = 0;
            return value;
        };
        device.playback_pcm = get_string("PlaybackPCM");
        device.capture_pcm = get_string("CapturePCM");
        device.playback_channels = get_int("PlaybackChannels");
        device.capture_channels = get_int("CaptureChannels");
        device.playback_priority = get_int("PlaybackPriority");
        device.capture_priority = get_int("CapturePriority");
        device.jack_control = get_string("JackControl");
        device.conflicting_devices = GetList("_conflictingdevs" + suffix);
        device.supported_devices = GetList("_supporteddevs" + suffix);
        devices_.push_back(std::move(device));
    }
}

const UcmDevice* UcmManager::FindDevice(const std::string& name) const {
    const auto it = std::find_if(devices_.begin(), devices_.end(),
                                 [&name](const UcmDevice& d) { return d.name == name; });
    return it != devices_.end() ? &*it : nullptr;
}

bool UcmManager::IsDeviceEnabledLocked(const std::string& name) {
    long status = 0;
    const int err = snd_use_case_geti(mgr_, ("_devstatus/" + name).c_str(), &status);
    return err >= 0 && status != 0;
}

bool UcmManager::IsDeviceEnabled(const std::string& name) {
    std::lock_guard guard(lock_);
    return IsDeviceEnabledLocked(name);
}

int UcmManager::EnableDevice(const std::string& name) {
    std::lock_guard guard(lock_);
    if (IsDeviceEnabledLocked(name)) return 0;
    // alsa-lib does not resolve conflicts on its own: a conflicting device that
    // stays enabled would leave e.g. both the speaker and the headphone path
    // switched on.
    if (const UcmDevice* device = FindDevice(name); device != nullptr) {
        for (const std::string& conflict : device->conflicting_devices) {
            if (IsDeviceEnabledLocked(conflict)) {
                LOG(INFO) << "card " << card_index_ << ": disabling \"" << conflict
                          << "\" which conflicts with \"" << name << "\"";
                SetLocked("_disdev", conflict.c_str());
            }
        }
    }
    return SetLocked("_enadev", name.c_str());
}

int UcmManager::DisableDevice(const std::string& name) {
    std::lock_guard guard(lock_);
    if (!IsDeviceEnabledLocked(name)) return 0;
    return SetLocked("_disdev", name.c_str());
}

}  // namespace aidl::android::hardware::audio::core::mainline::ucm
