/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "passthrough/SinkCapabilities.h"

#include <sstream>

namespace aidl::android::hardware::audio::core::mainline::passthrough {

namespace {

const char* ToString(SinkCapabilities::Source source) {
    switch (source) {
        case SinkCapabilities::Source::kNone:
            return "none";
        case SinkCapabilities::Source::kEld:
            return "ELD";
        case SinkCapabilities::Source::kProperty:
            return "property";
    }
    return "?";
}

}  // namespace

std::string SinkCapabilities::ToString() const {
    std::ostringstream os;
    os << "source=" << passthrough::ToString(source);
    if (!monitor_name.empty()) os << " monitor=\"" << monitor_name << "\"";
    os << " pcm_channels=" << max_pcm_channels << " formats={";
    bool first_format = true;
    for (const auto& [format, caps] : formats) {
        os << (first_format ? "" : ", ") << passthrough::ToString(format) << ":"
           << caps.max_channels << "ch@";
        bool first_rate = true;
        for (const uint32_t rate : caps.rates) {
            os << (first_rate ? "" : "/") << rate;
            first_rate = false;
        }
        first_format = false;
    }
    os << "}";
    return os.str();
}

SinkCapabilities SinkCapabilities::FromFormatList(const std::vector<EncodedFormat>& formats) {
    SinkCapabilities caps;
    caps.source = Source::kProperty;
    for (const EncodedFormat format : formats) {
        Format& entry = caps.formats[format];
        entry.max_channels = 8;
        for (const uint32_t rate : {32000, 44100, 48000, 88200, 96000, 176400, 192000}) {
            entry.rates.insert(rate);
        }
    }
    return caps;
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
