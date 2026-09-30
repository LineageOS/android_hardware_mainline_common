/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libhwdb/UdevSanitize.h>

#include <cctype>
#include <cstring>

namespace libhwdb {

namespace {

// Characters udev keeps verbatim in a device node name, see
// allow_listed_char_for_devnode() in systemd's src/basic/device-nodes.c.
bool IsAllowedChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isdigit(uc) || std::isalpha(uc)) {
        return true;
    }
    return std::strchr("#+-.:=@_", c) != nullptr && c != '\0';
}

// Additional characters allowed for rule input, UDEV_ALLOWED_CHARS_INPUT in
// systemd's src/udev/udev-format.h.
bool IsAllowedInputChar(char c) {
    return std::strchr("/ $%?,", c) != nullptr && c != '\0';
}

// Length of the UTF-8 sequence starting at `s`, or 0 if it is not a valid
// multi-byte sequence.
size_t Utf8SequenceLength(const std::string& s, size_t i) {
    const unsigned char lead = static_cast<unsigned char>(s[i]);
    size_t length = 0;
    if ((lead & 0xe0) == 0xc0) {
        length = 2;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
    } else {
        return 0;
    }
    if (i + length > s.size()) {
        return 0;
    }
    for (size_t k = 1; k < length; k++) {
        if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) {
            return 0;
        }
    }
    return length;
}

}  // namespace

/*
 * Reproduces what udev does to a sysfs attribute value substituted with
 * "$attr{...}" in a rule: udev_replace_chars(value, UDEV_ALLOWED_CHARS_INPUT)
 * in systemd's src/udev/udev-format.c. Every character that is neither
 * allow-listed, part of a "\x" escape nor part of a valid UTF-8 sequence is
 * replaced by '_'.
 *
 * hwdb entries are written from udev's output, so the lookup keys have to go
 * through the same transformation. Most importantly the device tree modalias
 * "of:NaccelerometerT(null)Csilan,sc7a20" becomes
 * "of:NaccelerometerT_null_Csilan,sc7a20".
 */
std::string UdevSanitize(const std::string& value) {
    std::string result = value;
    for (size_t i = 0; i < result.size();) {
        if (IsAllowedChar(result[i]) || IsAllowedInputChar(result[i])) {
            i++;
            continue;
        }
        if (result[i] == '\\' && i + 1 < result.size() && result[i + 1] == 'x') {
            i += 2;
            continue;
        }
        const size_t utf8_length = Utf8SequenceLength(result, i);
        if (utf8_length > 1) {
            i += utf8_length;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(result[i]))) {
            // ' ' is allow-listed for rule input, so whitespace becomes a
            // plain space instead of an underscore.
            result[i] = ' ';
        } else {
            result[i] = '_';
        }
        i++;
    }
    return result;
}

}  // namespace libhwdb
