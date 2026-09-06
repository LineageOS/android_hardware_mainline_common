/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "IioTypes.h"

namespace aidl::android::hardware::sensors::mainline {

// Parsed "scan_elements/<channel>_type" description, e.g. "le:s16/16>>0" or
// "le:s16/32X4>>0" (see iio_show_fixed_type() in the kernel).
struct IioScanType {
    bool big_endian = false;
    bool is_signed = false;
    int realbits = 0;
    int storagebits = 0;
    int shift = 0;
    int repeat = 1;

    static std::optional<IioScanType> Parse(const std::string& text);

    // Bytes of one element and of the whole (possibly repeated) channel.
    size_t ElementBytes() const { return static_cast<size_t>((storagebits + 7) / 8); }
    size_t TotalBytes() const { return ElementBytes() * static_cast<size_t>(repeat); }
    // Largest raw value representable with realbits.
    double MaxRawValue() const;
    std::string ToString() const;
};

/*
 * One IIO channel of a device: what we know about its sysfs attributes and, if
 * it can be part of a buffer scan, its scan element description.
 */
struct IioChannel {
    IioChannelId id;

    // Attribute (file name in the device directory) to read in poll mode:
    // "<key>_input" (processed value) or "<key>_raw". Empty if not readable.
    std::string poll_attribute;
    bool poll_is_processed = false;

    // Buffer scan element.
    bool has_scan_element = false;
    int scan_index = -1;
    IioScanType scan_type;
    // Byte offset of the channel within a scan, valid after ComputeScanLayout().
    size_t location = 0;
    bool enabled_in_scan = false;

    // Conversion attributes. Scale defaults to 1.0 and offset to 0.0 when the
    // attributes do not exist.
    double scale = 1.0;
    bool has_scale = false;
    double offset = 0.0;
    bool has_offset = false;

    bool IsTimestamp() const { return id.IsTimestamp(); }

    // Decodes element `element` of the channel from a scan buffer into a
    // sign-corrected raw integer (endianness, shift and realbits applied).
    int64_t DecodeRaw(const uint8_t* scan, int element = 0) const;
};

// Computes the byte offsets of the enabled channels of a scan in the same way
// as the kernel (iio_compute_scan_bytes): channels sorted by scan index, each
// aligned to its own storage size, the timestamp last, total size padded to
// the largest element. Returns the scan size in bytes (0 if nothing enabled).
size_t ComputeScanLayout(std::vector<IioChannel*>* enabled_channels);

}  // namespace aidl::android::hardware::sensors::mainline
