/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/sensors/SensorType.h>

#include <optional>
#include <string>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

/*
 * Identity of an IIO channel as encoded in sysfs attribute names:
 *   in_<type>[<index>][_<modifier>]_<postfix>
 * e.g. "in_accel_x_raw" -> {type="accel", index=-1, modifier="x"}
 *      "in_proximity0_raw" -> {type="proximity", index=0, modifier=""}
 *      "in_accel_scale" -> {type="accel", index=-1, modifier=""} (shared by type)
 */
struct IioChannelId {
    std::string type;
    int index = -1;
    std::string modifier;

    // Attribute name prefix, e.g. "in_accel_x" or "in_proximity0".
    std::string Key() const;
    // Prefix of the attributes shared by all channels of the type, e.g.
    // "in_accel".
    std::string TypeKey() const;
    bool IsTimestamp() const { return type == "timestamp"; }
    bool operator==(const IioChannelId& other) const {
        return type == other.type && index == other.index && modifier == other.modifier;
    }
};

struct IioParsedAttribute {
    IioChannelId id;
    // Attribute postfix, e.g. "raw", "input", "scale", "en", "index", "type",
    // "sampling_frequency", "mount_matrix", "scale_available", ...
    std::string postfix;
};

// Parses an "in_*" attribute file name. Returns nullopt for names that do not
// follow the IIO channel attribute scheme.
std::optional<IioParsedAttribute> ParseIioAttributeName(const std::string& filename);

// Sensor type derived from a channel group.
struct IioSensorSpec {
    ::aidl::android::hardware::sensors::SensorType android_type;
    // IIO channel type, e.g. "accel".
    std::string iio_type;
    // Modifiers of the channels making up the sensor, in payload order.
    // {"x", "y", "z"} for 3-axis sensors, {""} for scalar ones.
    std::vector<std::string> modifiers;
    // Channel index (-1 = unindexed).
    int index = -1;
    // Factor converting IIO units (after scale/offset) to Android units.
    double unit_factor = 1.0;
    // Payload of the Android event.
    enum class Payload {
        kVec3,
        kScalar,
        kProximity,   // scalar, with near/far or distance conversion
        kQuaternion,  // one channel with 4 repeated elements
        kStepCount,
    } payload = Payload::kScalar;
};

// Device specific behaviours which cannot be derived from sysfs.
struct IioDeviceQuirks {
    // Proximity raw value is a distance: metres = raw * scale + offset.
    bool proximity_is_distance = false;
    // Pressure channel already reports hPa instead of the IIO kPa.
    bool pressure_in_hpa = false;
    // Fixed near level for the proximity channel (e.g. HID presence: 1).
    std::optional<double> proximity_near_level;
    // Ignore the "offset" attribute of every channel.
    bool ignore_offset = false;
    // The buffer timestamp channel does not carry nanoseconds.
    bool ignore_timestamp_channel = false;
    // Rotation vector flavour for "rot"/"quaternion" channels.
    ::aidl::android::hardware::sensors::SensorType rotation_vector_type =
            ::aidl::android::hardware::sensors::SensorType::ROTATION_VECTOR;
};

IioDeviceQuirks GetIioDeviceQuirks(const std::string& device_name);

// Sensor specs that can be built from the readable channels of a device.
// A "temp" channel is only mapped when the device exposes no other sensor
// type (the die temperature of an IMU is not an ambient temperature) unless
// `expose_temperature` is set.
std::vector<IioSensorSpec> MatchIioSensorSpecs(const std::vector<IioChannelId>& channels,
                                               const IioDeviceQuirks& quirks,
                                               bool expose_temperature);

// Returns the human readable vendor part of a device tree compatible string,
// e.g. "bosch,bmi160" -> "Bosch". Empty if unavailable.
std::string VendorFromCompatible(const std::string& compatible);
// Returns the model part of a compatible string, e.g. "bosch,bmi160" -> "bmi160".
std::string ModelFromCompatible(const std::string& compatible);
// Returns a vendor guess from a modalias, e.g. "platform:HID-SENSOR-200073" ->
// "HID". Empty if unknown.
std::string VendorFromModalias(const std::string& modalias);
// True for names such as "3-000c" or "spi0.0" that drivers use when they have
// no better name (e.g. ak8975 probed from device tree).
bool LooksLikeBusAddress(const std::string& name);

// Guesses a sensor type from free-form names (device name, of_node/name,
// compatible). Only used for logging hints and as a name-based fallback when
// the channels of a device cannot be classified.
std::optional<::aidl::android::hardware::sensors::SensorType> GuessSensorTypeFromName(
        const std::string& name);

// Short name of an IIO type used in configuration keys ("accel", "gyro", ...).
std::string ConfigNameForSensorType(::aidl::android::hardware::sensors::SensorType type);

}  // namespace aidl::android::hardware::sensors::mainline
