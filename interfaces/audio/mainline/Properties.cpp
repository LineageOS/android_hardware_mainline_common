/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Properties"

#include "Properties.h"

#include <algorithm>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

namespace aidl::android::hardware::audio::core::mainline {

namespace {

std::string Key(const char* suffix) {
    return std::string(Properties::kPrefix) + suffix;
}

std::vector<std::string> SplitList(const std::string& value) {
    std::vector<std::string> result;
    for (const auto& item : ::android::base::Split(value, ",")) {
        std::string trimmed = ::android::base::Trim(item);
        if (!trimmed.empty()) result.push_back(std::move(trimmed));
    }
    return result;
}

int ClampPercent(int value) {
    return std::clamp(value, 0, 100);
}

}  // namespace

Properties Properties::Load() {
    using ::android::base::GetBoolProperty;
    using ::android::base::GetIntProperty;
    using ::android::base::GetProperty;

    Properties props;
    props.cards = SplitList(GetProperty(Key("cards"), ""));
    props.primary_card = ::android::base::Trim(GetProperty(Key("primary_card"), ""));
    props.include_usb_cards = GetBoolProperty(Key("include_usb_cards"), props.include_usb_cards);
    props.ucm_enabled = GetBoolProperty(Key("ucm.enabled"), props.ucm_enabled);
    props.ucm_verb = GetProperty(Key("ucm.verb"), props.ucm_verb);
    props.mixer_init = GetBoolProperty(Key("mixer.init"), props.mixer_init);
    props.mixer_playback_percent = ClampPercent(
            GetIntProperty(Key("mixer.playback_percent"), props.mixer_playback_percent));
    props.mixer_capture_percent =
            ClampPercent(GetIntProperty(Key("mixer.capture_percent"), props.mixer_capture_percent));
    props.latency_ms = std::clamp(GetIntProperty(Key("latency_ms"), props.latency_ms), 5, 500);
    props.multichannel = GetBoolProperty(Key("multichannel"), props.multichannel);
    props.verbose_logging = GetBoolProperty(Key("log.verbose"), props.verbose_logging);

    LOG(INFO) << "loaded properties: " << props.ToString();
    return props;
}

std::string Properties::ToString() const {
    std::ostringstream os;
    os << "cards=[" << ::android::base::Join(cards, ",") << "]"
       << " primary_card=\"" << primary_card << "\""
       << " include_usb_cards=" << include_usb_cards << " ucm.enabled=" << ucm_enabled
       << " ucm.verb=\"" << ucm_verb << "\""
       << " mixer.init=" << mixer_init << " mixer.playback_percent=" << mixer_playback_percent
       << " mixer.capture_percent=" << mixer_capture_percent << " latency_ms=" << latency_ms
       << " multichannel=" << multichannel << " log.verbose=" << verbose_logging;
    return os.str();
}

}  // namespace aidl::android::hardware::audio::core::mainline
