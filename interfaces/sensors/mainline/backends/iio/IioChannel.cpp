/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsIio"

#include "IioChannel.h"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aidl::android::hardware::sensors::mainline {

std::optional<IioScanType> IioScanType::Parse(const std::string& text) {
    // [be|le]:[s|u]<realbits>/<storagebits>[X<repeat>]>><shift>
    IioScanType type;
    size_t pos = 0;
    if (text.compare(0, 3, "be:") == 0) {
        type.big_endian = true;
    } else if (text.compare(0, 3, "le:") == 0) {
        type.big_endian = false;
    } else {
        return std::nullopt;
    }
    pos = 3;
    if (pos >= text.size()) {
        return std::nullopt;
    }
    if (text[pos] == 's') {
        type.is_signed = true;
    } else if (text[pos] == 'u') {
        type.is_signed = false;
    } else {
        return std::nullopt;
    }
    pos++;

    auto parse_number = [&](char stop, int* out) -> bool {
        size_t end = pos;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
            end++;
        }
        if (end == pos) {
            return false;
        }
        if (!::android::base::ParseInt(text.substr(pos, end - pos), out)) {
            return false;
        }
        pos = end;
        if (stop != '\0') {
            if (pos >= text.size() || text[pos] != stop) {
                return false;
            }
            pos++;
        }
        return true;
    };

    if (!parse_number('/', &type.realbits)) {
        return std::nullopt;
    }
    if (!parse_number('\0', &type.storagebits)) {
        return std::nullopt;
    }
    if (pos < text.size() && text[pos] == 'X') {
        pos++;
        if (!parse_number('\0', &type.repeat)) {
            return std::nullopt;
        }
    }
    if (text.compare(pos, 2, ">>") == 0) {
        pos += 2;
        if (!parse_number('\0', &type.shift)) {
            return std::nullopt;
        }
    }
    if (type.repeat < 1) {
        type.repeat = 1;
    }
    if (type.storagebits <= 0 || type.storagebits > 64 || type.realbits <= 0 ||
        type.realbits > 64 || type.shift < 0 || type.realbits + type.shift > type.storagebits) {
        return std::nullopt;
    }
    return type;
}

double IioScanType::MaxRawValue() const {
    int bits = is_signed ? realbits - 1 : realbits;
    if (bits <= 0) {
        return 0.0;
    }
    return std::ldexp(1.0, bits) - 1.0;
}

std::string IioScanType::ToString() const {
    if (repeat > 1) {
        return ::android::base::StringPrintf("%s:%c%d/%dX%d>>%d", big_endian ? "be" : "le",
                                             is_signed ? 's' : 'u', realbits, storagebits, repeat,
                                             shift);
    }
    return ::android::base::StringPrintf("%s:%c%d/%d>>%d", big_endian ? "be" : "le",
                                         is_signed ? 's' : 'u', realbits, storagebits, shift);
}

int64_t IioChannel::DecodeRaw(const uint8_t* scan, int element) const {
    const size_t bytes = scan_type.ElementBytes();
    const uint8_t* src = scan + location + static_cast<size_t>(element) * bytes;

    uint64_t value = 0;
    if (scan_type.big_endian) {
        for (size_t i = 0; i < bytes; i++) {
            value = (value << 8) | src[i];
        }
    } else {
        for (size_t i = bytes; i > 0; i--) {
            value = (value << 8) | src[i - 1];
        }
    }

    value >>= scan_type.shift;
    const int realbits = scan_type.realbits;
    if (realbits < 64) {
        const uint64_t mask = (1ULL << realbits) - 1;
        value &= mask;
        if (scan_type.is_signed && (value & (1ULL << (realbits - 1)))) {
            value |= ~mask;
        }
    }
    return static_cast<int64_t>(value);
}

size_t ComputeScanLayout(std::vector<IioChannel*>* enabled_channels) {
    std::vector<IioChannel*>& channels = *enabled_channels;
    // Timestamp always last, others by scan index.
    std::sort(channels.begin(), channels.end(), [](const IioChannel* a, const IioChannel* b) {
        if (a->IsTimestamp() != b->IsTimestamp()) {
            return !a->IsTimestamp();
        }
        return a->scan_index < b->scan_index;
    });

    size_t bytes = 0;
    size_t largest = 1;
    for (IioChannel* channel : channels) {
        const size_t length = channel->scan_type.TotalBytes();
        if (length == 0) {
            continue;
        }
        if (bytes % length != 0) {
            bytes += length - (bytes % length);
        }
        channel->location = bytes;
        bytes += length;
        largest = std::max(largest, length);
    }
    if (bytes % largest != 0) {
        bytes += largest - (bytes % largest);
    }
    return bytes;
}

}  // namespace aidl::android::hardware::sensors::mainline
