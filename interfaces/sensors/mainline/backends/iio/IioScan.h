/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace aidl::android::hardware::sensors::mainline::iio {

struct ScanType {
    bool big_endian = false;
    bool is_signed = false;
    uint8_t real_bits = 0;
    uint8_t storage_bits = 0;
    uint16_t repeat = 1;
    uint8_t shift = 0;
};

struct ScanElement {
    int index = -1;
    ScanType type;
    size_t offset = 0;
    bool is_timestamp = false;
};

struct DecodedValue {
    uint64_t bits = 0;
    bool is_signed = false;

    double AsDouble() const;
};

enum class Unit {
    kAcceleration,
    kAngularVelocity,
    kMagneticFieldGauss,
    kPressureKilopascal,
    kRelativeHumidityMilliPercent,
    kTemperatureMilliCelsius,
    kProximityMeters,
    kUnchanged,
};

bool ParseScanType(std::string_view text, ScanType* type);
std::optional<size_t> ComputeScanLayout(std::vector<ScanElement>* elements);
std::optional<DecodedValue> DecodeScanValue(const uint8_t* data, size_t size, const ScanType& type,
                                            size_t repeat_index = 0);
double ConvertUnit(double value, Unit unit);

}  // namespace aidl::android::hardware::sensors::mainline::iio
