/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsIio"

#include "IioTypes.h"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <libsensors_common/SensorTypes.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace aidl::android::hardware::sensors::mainline {

namespace {

// iio_chan_type_name_spec[] of drivers/iio/industrialio-core.c.
const char* const kChannelTypes[] = {
        "voltage",
        "current",
        "power",
        "accel",
        "anglvel",
        "magn",
        "illuminance",
        "intensity",
        "proximity",
        "temp",
        "incli",
        "rot",
        "angl",
        "timestamp",
        "capacitance",
        "altvoltage",
        "cct",
        "pressure",
        "humidityrelative",
        "activity",
        "steps",
        "energy",
        "distance",
        "velocity",
        "concentration",
        "resistance",
        "ph",
        "uvindex",
        "electricalconductivity",
        "count",
        "index",
        "gravity",
        "positionrelative",
        "phase",
        "massconcentration",
        "deltaangl",
        "deltavelocity",
        "colortemp",
        "chromaticity",
        "attention",
        "altcurrent",
};

// iio_modifier_names[] of drivers/iio/industrialio-core.c.
const char* const kModifiers[] = {
        "x",
        "y",
        "z",
        "x&y",
        "x&z",
        "y&z",
        "x&y&z",
        "x|y",
        "x|z",
        "y|z",
        "x|y|z",
        "both",
        "ir",
        "sqrt(x^2+y^2)",
        "x^2+y^2+z^2",
        "clear",
        "red",
        "green",
        "blue",
        "quaternion",
        "ambient",
        "object",
        "from_north_magnetic",
        "from_north_true",
        "from_north_magnetic_tilt_comp",
        "from_north_true_tilt_comp",
        "running",
        "jogging",
        "walking",
        "still",
        "sqrt(x^2+y^2+z^2)",
        "i",
        "q",
        "co2",
        "voc",
        "uv",
        "duv",
        "pm1",
        "pm2p5",
        "pm4",
        "pm10",
        "ethanol",
        "h2",
        "o2",
        "linear_x",
        "linear_y",
        "linear_z",
        "pitch",
        "yaw",
        "roll",
        "uva",
        "uvb",
        "rms",
        "active",
        "reactive",
        "apparent",
};

// Attribute postfixes we recognise, longest first so that e.g.
// "sampling_frequency_available" wins over "sampling_frequency".
const char* const kPostfixes[] = {
        "sampling_frequency_available",
        "integration_time_available",
        "calibbias_available",
        "hysteresis_relative",
        "sampling_frequency",
        "scale_available",
        "integration_time",
        "mount_matrix",
        "hysteresis",
        "calibscale",
        "calibbias",
        "nearlevel",
        "offset",
        "scale",
        "input",
        "label",
        "index",
        "type",
        "raw",
        "en",
};

struct QuirkEntry {
    // Matched with a prefix comparison against the device name.
    const char* name_prefix;
    IioDeviceQuirks quirks;
};

IioDeviceQuirks MakeSmgrProximityQuirks() {
    IioDeviceQuirks q;
    // drivers/iio/proximity/qcom_smgr_prox.c: distance in metres is
    // raw * scale + offset (scale is negative).
    q.proximity_is_distance = true;
    q.ignore_timestamp_channel = true;
    return q;
}

IioDeviceQuirks MakeSmgrPressureQuirks() {
    IioDeviceQuirks q;
    // drivers/iio/pressure/qcom_smgr_pressure.c reports hPa in Q16 and a
    // bogus offset attribute.
    q.pressure_in_hpa = true;
    q.ignore_offset = true;
    q.ignore_timestamp_channel = true;
    return q;
}

IioDeviceQuirks MakeSmgrQuirks() {
    IioDeviceQuirks q;
    // drivers/iio/common/qcom_smgr: timestamp channel carries 32-bit SMGR
    // ticks, not nanoseconds.
    q.ignore_timestamp_channel = true;
    return q;
}

IioDeviceQuirks MakeHidProxQuirks() {
    IioDeviceQuirks q;
    // drivers/iio/light/hid-sensor-prox.c: human presence 1 = near.
    q.proximity_near_level = 1.0;
    return q;
}

IioDeviceQuirks MakeRotationQuirks(::aidl::android::hardware::sensors::SensorType type) {
    IioDeviceQuirks q;
    q.rotation_vector_type = type;
    return q;
}

const QuirkEntry kQuirks[] = {
        {"qcom-smgr-prox", MakeSmgrProximityQuirks()},
        {"qcom-smgr-pressure", MakeSmgrPressureQuirks()},
        {"qcom-smgr-", MakeSmgrQuirks()},
        {"prox", MakeHidProxQuirks()},
        // drivers/iio/orientation/hid-sensor-rotation.c device names.
        {"relative_orientation",
         MakeRotationQuirks(::aidl::android::hardware::sensors::SensorType::GAME_ROTATION_VECTOR)},
        {"geomagnetic_orientation",
         MakeRotationQuirks(
                 ::aidl::android::hardware::sensors::SensorType::GEOMAGNETIC_ROTATION_VECTOR)},
        {"dev_rotation",
         MakeRotationQuirks(::aidl::android::hardware::sensors::SensorType::ROTATION_VECTOR)},
};

bool HasChannel(const std::vector<IioChannelId>& channels, const std::string& type,
                const std::string& modifier, int index) {
    return std::any_of(channels.begin(), channels.end(), [&](const IioChannelId& id) {
        return id.type == type && id.modifier == modifier && id.index == index;
    });
}

// Returns the index of the first channel of `type` with the given modifier,
// preferring unindexed channels, then index 0, then the lowest index.
std::optional<int> FindChannelIndex(const std::vector<IioChannelId>& channels,
                                    const std::string& type, const std::string& modifier) {
    std::optional<int> best;
    for (const auto& id : channels) {
        if (id.type != type || id.modifier != modifier) {
            continue;
        }
        if (!best.has_value() || id.index < *best) {
            best = id.index;
        }
    }
    return best;
}

}  // namespace

std::string IioChannelId::Key() const {
    std::string key = "in_" + type;
    if (index >= 0) {
        key += std::to_string(index);
    }
    if (!modifier.empty()) {
        key += "_" + modifier;
    }
    return key;
}

std::string IioChannelId::TypeKey() const {
    return "in_" + type;
}

std::optional<IioParsedAttribute> ParseIioAttributeName(const std::string& filename) {
    if (!::android::base::StartsWith(filename, "in_")) {
        return std::nullopt;
    }
    std::string body = filename.substr(3);

    // Postfix.
    std::string postfix;
    for (const char* candidate : kPostfixes) {
        std::string suffix = std::string("_") + candidate;
        if (::android::base::EndsWith(body, suffix)) {
            postfix = candidate;
            body = body.substr(0, body.size() - suffix.size());
            break;
        }
    }
    if (postfix.empty()) {
        return std::nullopt;
    }

    // Type: longest matching prefix.
    IioParsedAttribute result;
    result.postfix = postfix;
    size_t best_len = 0;
    for (const char* type : kChannelTypes) {
        size_t len = strlen(type);
        if (len > best_len && ::android::base::StartsWith(body, type)) {
            best_len = len;
            result.id.type = type;
        }
    }
    if (best_len == 0) {
        return std::nullopt;
    }
    body = body.substr(best_len);

    // Optional index.
    size_t digits = 0;
    while (digits < body.size() && std::isdigit(static_cast<unsigned char>(body[digits]))) {
        digits++;
    }
    if (digits > 0) {
        int index = -1;
        if (!::android::base::ParseInt(body.substr(0, digits), &index)) {
            return std::nullopt;
        }
        result.id.index = index;
        body = body.substr(digits);
    }

    // Optional modifier.
    if (body.empty()) {
        return result;
    }
    if (body[0] != '_') {
        return std::nullopt;
    }
    body = body.substr(1);
    for (const char* modifier : kModifiers) {
        if (body == modifier) {
            result.id.modifier = modifier;
            return result;
        }
    }
    // Driver specific extended names are not supported.
    return std::nullopt;
}

IioDeviceQuirks GetIioDeviceQuirks(const std::string& device_name) {
    for (const auto& entry : kQuirks) {
        if (::android::base::StartsWith(device_name, entry.name_prefix)) {
            LOG(DEBUG) << "Applying quirks '" << entry.name_prefix << "' to device '" << device_name
                       << "'";
            return entry.quirks;
        }
    }
    return IioDeviceQuirks();
}

std::vector<IioSensorSpec> MatchIioSensorSpecs(const std::vector<IioChannelId>& channels,
                                               const IioDeviceQuirks& quirks,
                                               bool expose_temperature) {
    using ::aidl::android::hardware::sensors::SensorType;
    std::vector<IioSensorSpec> specs;

    auto add_vec3 = [&](const char* iio_type, SensorType type, double unit_factor) {
        if (HasChannel(channels, iio_type, "x", -1) && HasChannel(channels, iio_type, "y", -1) &&
            HasChannel(channels, iio_type, "z", -1)) {
            IioSensorSpec spec;
            spec.android_type = type;
            spec.iio_type = iio_type;
            spec.modifiers = {"x", "y", "z"};
            spec.unit_factor = unit_factor;
            spec.payload = IioSensorSpec::Payload::kVec3;
            specs.push_back(spec);
        }
    };
    auto add_scalar = [&](const char* iio_type, const char* modifier, SensorType type,
                          double unit_factor, IioSensorSpec::Payload payload) {
        auto index = FindChannelIndex(channels, iio_type, modifier);
        if (!index.has_value()) {
            return false;
        }
        IioSensorSpec spec;
        spec.android_type = type;
        spec.iio_type = iio_type;
        spec.modifiers = {modifier};
        spec.index = *index;
        spec.unit_factor = unit_factor;
        spec.payload = payload;
        specs.push_back(spec);
        return true;
    };

    // Units: IIO accel is m/s^2, anglvel rad/s, magn Gauss (Android: uT),
    // gravity m/s^2.
    add_vec3("accel", SensorType::ACCELEROMETER, 1.0);
    add_vec3("anglvel", SensorType::GYROSCOPE, 1.0);
    add_vec3("magn", SensorType::MAGNETIC_FIELD, 100.0);
    add_vec3("gravity", SensorType::GRAVITY, 1.0);

    // Light: illuminance is lux. HID sensors only expose "intensity_both"
    // which they also fill with lux (see iio-sensor-proxy).
    if (!add_scalar("illuminance", "", SensorType::LIGHT, 1.0, IioSensorSpec::Payload::kScalar)) {
        if (!add_scalar("intensity", "both", SensorType::LIGHT, 1.0,
                        IioSensorSpec::Payload::kScalar)) {
            add_scalar("intensity", "clear", SensorType::LIGHT, 1.0,
                       IioSensorSpec::Payload::kScalar);
        }
    }

    add_scalar("proximity", "", SensorType::PROXIMITY, 1.0, IioSensorSpec::Payload::kProximity);

    // Pressure: kPa -> hPa.
    add_scalar("pressure", "", SensorType::PRESSURE, quirks.pressure_in_hpa ? 1.0 : 10.0,
               IioSensorSpec::Payload::kScalar);
    // Humidity: milli percent -> percent.
    add_scalar("humidityrelative", "", SensorType::RELATIVE_HUMIDITY, 0.001,
               IioSensorSpec::Payload::kScalar);

    // Temperature: milli degrees Celsius -> degrees Celsius. Only exposed for
    // dedicated temperature sensors.
    bool has_other_types = std::any_of(
            channels.begin(), channels.end(),
            [](const IioChannelId& id) { return id.type != "temp" && !id.IsTimestamp(); });
    if (expose_temperature || !has_other_types) {
        if (!add_scalar("temp", "ambient", SensorType::AMBIENT_TEMPERATURE, 0.001,
                        IioSensorSpec::Payload::kScalar)) {
            add_scalar("temp", "", SensorType::AMBIENT_TEMPERATURE, 0.001,
                       IioSensorSpec::Payload::kScalar);
        }
    }

    // Rotation quaternion (HID orientation sensors).
    add_scalar("rot", "quaternion", quirks.rotation_vector_type, 1.0,
               IioSensorSpec::Payload::kQuaternion);

    // Hinge angle: radians -> degrees.
    add_scalar("angl", "", SensorType::HINGE_ANGLE, 180.0 / 3.14159265358979,
               IioSensorSpec::Payload::kScalar);

    add_scalar("steps", "", SensorType::STEP_COUNTER, 1.0, IioSensorSpec::Payload::kStepCount);

    return specs;
}

std::string VendorFromCompatible(const std::string& compatible) {
    size_t comma = compatible.find(',');
    if (comma == std::string::npos || comma == 0) {
        return "";
    }
    std::string vendor = compatible.substr(0, comma);
    vendor[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(vendor[0])));
    return vendor;
}

std::string ModelFromCompatible(const std::string& compatible) {
    size_t comma = compatible.find(',');
    if (comma == std::string::npos) {
        return compatible;
    }
    return compatible.substr(comma + 1);
}

std::string VendorFromModalias(const std::string& modalias) {
    if (::android::base::StartsWith(modalias, "platform:HID-SENSOR-")) {
        return "HID";
    }
    if (::android::base::StartsWith(modalias, "acpi:")) {
        return "ACPI";
    }
    if (::android::base::StartsWith(modalias, "of:")) {
        // of:N<name>T<type>C<compatible>
        size_t c = modalias.find('C');
        if (c != std::string::npos) {
            return VendorFromCompatible(modalias.substr(c + 1));
        }
    }
    return "";
}

bool LooksLikeBusAddress(const std::string& name) {
    // "3-000c" (i2c), "spi0.0", "spi1.2".
    size_t dash = name.find('-');
    if (dash != std::string::npos && dash > 0) {
        bool ok = true;
        for (size_t i = 0; i < name.size(); i++) {
            if (i == dash) continue;
            if (!std::isxdigit(static_cast<unsigned char>(name[i]))) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    if (::android::base::StartsWith(name, "spi") && name.find('.') != std::string::npos) {
        return true;
    }
    return false;
}

std::optional<::aidl::android::hardware::sensors::SensorType> GuessSensorTypeFromName(
        const std::string& name) {
    using ::aidl::android::hardware::sensors::SensorType;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower.find("accel") != std::string::npos) return SensorType::ACCELEROMETER;
    if (lower.find("gyro") != std::string::npos || lower.find("anglvel") != std::string::npos) {
        return SensorType::GYROSCOPE;
    }
    if (lower.find("magn") != std::string::npos || lower.find("compass") != std::string::npos) {
        return SensorType::MAGNETIC_FIELD;
    }
    if (lower.find("light") != std::string::npos || lower.find("als") != std::string::npos ||
        lower.find("illuminance") != std::string::npos) {
        return SensorType::LIGHT;
    }
    if (lower.find("prox") != std::string::npos) return SensorType::PROXIMITY;
    if (lower.find("press") != std::string::npos || lower.find("baro") != std::string::npos) {
        return SensorType::PRESSURE;
    }
    if (lower.find("humid") != std::string::npos) return SensorType::RELATIVE_HUMIDITY;
    if (lower.find("temp") != std::string::npos) return SensorType::AMBIENT_TEMPERATURE;
    return std::nullopt;
}

std::string ConfigNameForSensorType(::aidl::android::hardware::sensors::SensorType type) {
    auto traits = GetSensorTypeTraits(type);
    return traits ? traits->config_name : std::to_string(static_cast<int32_t>(type));
}

}  // namespace aidl::android::hardware::sensors::mainline
