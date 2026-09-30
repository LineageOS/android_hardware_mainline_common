/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "passthrough/Format.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// What an HDMI sink decodes, as far as passthrough is concerned.
struct SinkCapabilities {
    enum class Source {
        kNone,      // Nothing known: no passthrough.
        kEld,       // Short audio descriptors of the sink's ELD.
        kProperty,  // hdmi.passthrough_formats, for sinks with a missing or wrong ELD.
    };

    struct Format {
        // Content sample rates the sink announces for the format.
        std::set<uint32_t> rates;
        unsigned int max_channels = 0;
    };

    Source source = Source::kNone;
    std::string monitor_name;
    std::map<EncodedFormat, Format> formats;
    // Highest channel count of linear PCM, 0 when not announced.
    unsigned int max_pcm_channels = 0;

    bool Supports(EncodedFormat format) const { return formats.count(format) != 0; }
    std::string ToString() const;

    // Capabilities forced by a list of formats: every rate, eight channels.
    static SinkCapabilities FromFormatList(const std::vector<EncodedFormat>& formats);
};

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
