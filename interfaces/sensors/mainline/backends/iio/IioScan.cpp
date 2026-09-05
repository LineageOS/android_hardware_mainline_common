/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IioScan.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>

namespace aidl::android::hardware::sensors::mainline::iio {
namespace {

bool ParseUnsigned(std::string_view text, unsigned int* value) {
    if (text.empty()) return false;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *value);
    return error == std::errc() && end == text.data() + text.size();
}

size_t Align(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

double DecodedValue::AsDouble() const {
    if (!is_signed) return static_cast<double>(bits);
    return static_cast<double>(static_cast<int64_t>(bits));
}

bool ParseScanType(std::string_view text, ScanType* type) {
    if (type == nullptr || text.size() < 10) return false;

    const size_t colon = text.find(':');
    const size_t slash = text.find('/', colon + 1);
    const size_t shift = text.find(">>", slash + 1);
    if (colon == std::string_view::npos || slash == std::string_view::npos ||
        shift == std::string_view::npos || colon + 2 >= slash) {
        return false;
    }

    const std::string_view endian = text.substr(0, colon);
    if (endian != "le" && endian != "be") return false;
    const char sign = text[colon + 1];
    if (sign != 's' && sign != 'u') return false;

    unsigned int real_bits;
    unsigned int storage_bits;
    unsigned int repeat = 1;
    unsigned int right_shift;
    if (!ParseUnsigned(text.substr(colon + 2, slash - colon - 2), &real_bits) ||
        !ParseUnsigned(text.substr(shift + 2), &right_shift)) {
        return false;
    }

    std::string_view storage = text.substr(slash + 1, shift - slash - 1);
    const size_t repeat_marker = storage.find('X');
    if (repeat_marker != std::string_view::npos) {
        if (!ParseUnsigned(storage.substr(repeat_marker + 1), &repeat)) return false;
        storage = storage.substr(0, repeat_marker);
    }
    if (!ParseUnsigned(storage, &storage_bits) || real_bits == 0 || storage_bits == 0 ||
        storage_bits > 64 || storage_bits % 8 != 0 || real_bits > storage_bits || repeat == 0 ||
        repeat > std::numeric_limits<uint16_t>::max() || right_shift >= storage_bits ||
        real_bits + right_shift > storage_bits) {
        return false;
    }

    *type = {
            .big_endian = endian == "be",
            .is_signed = sign == 's',
            .real_bits = static_cast<uint8_t>(real_bits),
            .storage_bits = static_cast<uint8_t>(storage_bits),
            .repeat = static_cast<uint16_t>(repeat),
            .shift = static_cast<uint8_t>(right_shift),
    };
    return true;
}

std::optional<size_t> ComputeScanLayout(std::vector<ScanElement>* elements) {
    if (elements == nullptr || elements->empty()) return std::nullopt;
    std::stable_sort(elements->begin(), elements->end(),
                     [](const ScanElement& left, const ScanElement& right) {
                         if (left.is_timestamp != right.is_timestamp) return !left.is_timestamp;
                         return left.index < right.index;
                     });

    size_t offset = 0;
    size_t largest = 0;
    std::set<int> indices;
    for (auto& element : *elements) {
        const size_t width = element.type.storage_bits / 8 * element.type.repeat;
        if (element.index < 0 || !indices.insert(element.index).second || width == 0)
            return std::nullopt;
        offset = Align(offset, width);
        element.offset = offset;
        if (offset > std::numeric_limits<size_t>::max() - width) return std::nullopt;
        offset += width;
        largest = std::max(largest, width);
    }
    return Align(offset, largest);
}

std::optional<DecodedValue> DecodeScanValue(const uint8_t* data, size_t size, const ScanType& type,
                                            size_t repeat_index) {
    const size_t width = type.storage_bits / 8;
    if (data == nullptr || width == 0 || width > 8 || repeat_index >= type.repeat ||
        size < width * (repeat_index + 1) || type.real_bits == 0 || type.real_bits > 64) {
        return std::nullopt;
    }

    data += width * repeat_index;
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
        const size_t byte = type.big_endian ? i : width - i - 1;
        value = (value << 8) | data[byte];
    }
    value >>= type.shift;

    const uint64_t mask = type.real_bits == 64 ? std::numeric_limits<uint64_t>::max()
                                               : (uint64_t{1} << type.real_bits) - 1;
    value &= mask;
    if (type.is_signed && type.real_bits < 64 && (value & (uint64_t{1} << (type.real_bits - 1)))) {
        value |= ~mask;
    }
    return DecodedValue{.bits = value, .is_signed = type.is_signed};
}

double ConvertUnit(double value, Unit unit) {
    switch (unit) {
        case Unit::kMagneticFieldGauss:
            return value * 100.0;
        case Unit::kPressureKilopascal:
            return value * 10.0;
        case Unit::kRelativeHumidityMilliPercent:
        case Unit::kTemperatureMilliCelsius:
            return value / 1000.0;
        case Unit::kProximityMeters:
            return value * 100.0;
        case Unit::kAcceleration:
        case Unit::kAngularVelocity:
        case Unit::kUnchanged:
            return value;
    }
}

}  // namespace aidl::android::hardware::sensors::mainline::iio
