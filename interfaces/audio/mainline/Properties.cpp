/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Properties"

#include "Properties.h"

#include <algorithm>
#include <set>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/parseint.h>
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

std::set<int> ParseIntSet(const std::string& value) {
    std::set<int> result;
    for (const auto& item : ::android::base::Split(value, ",")) {
        int v;
        if (::android::base::ParseInt(::android::base::Trim(item), &v)) {
            result.insert(v);
        }
    }
    return result;
}

}  // namespace

Properties Properties::Load() {
    using ::android::base::GetBoolProperty;
    using ::android::base::GetIntProperty;
    using ::android::base::GetProperty;

    Properties props;
    props.cards = SplitList(GetProperty(Key("cards"), ""));
    props.wait_for_cards_ms =
            std::clamp(GetIntProperty(Key("wait_for_cards_ms"), props.wait_for_cards_ms), 0, 60000);
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
       << " wait_for_cards_ms=" << wait_for_cards_ms << " primary_card=\"" << primary_card << "\""
       << " include_usb_cards=" << include_usb_cards << " ucm.enabled=" << ucm_enabled
       << " ucm.verb=\"" << ucm_verb << "\""
       << " mixer.init=" << mixer_init << " mixer.playback_percent=" << mixer_playback_percent
       << " mixer.capture_percent=" << mixer_capture_percent << " latency_ms=" << latency_ms
       << " multichannel=" << multichannel << " log.verbose=" << verbose_logging;
    return os.str();
}

Properties::CardProperties Properties::LoadCardProperties(const std::string& card_id,
                                                          int card_index,
                                                          const std::string& card_name) {
    using ::android::base::GetProperty;

    CardProperties merged;
    std::string name_with_underscores = card_name;
    std::replace(name_with_underscores.begin(), name_with_underscores.end(), ' ', '_');

    for (const std::string& selector :
         {card_id, std::to_string(card_index), name_with_underscores}) {
        const std::string prefix = std::string(kPrefix) + "card." + selector + ".";
        auto rates = ParseIntSet(GetProperty(prefix + "rates", ""));
        merged.rates.insert(rates.begin(), rates.end());
        auto bits = ParseIntSet(GetProperty(prefix + "bits", ""));
        merged.bits.insert(bits.begin(), bits.end());
    }
    return merged;
}

}  // namespace aidl::android::hardware::audio::core::mainline
