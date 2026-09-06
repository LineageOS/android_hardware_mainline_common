/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace libhwdb {
class Hwdb;
}

namespace aidl::android::hardware::sensors::mainline {

/*
 * Access to the systemd compatible sensor hardware database (60-sensor.hwdb).
 *
 * The database maps a sensor, identified by the modalias of its parent device
 * and the DMI modalias of the machine, to properties such as
 * ACCEL_MOUNT_MATRIX, ACCEL_LOCATION and PROXIMITY_NEAR_LEVEL. It is maintained
 * by the Linux community and can be shipped unmodified on the device in one of:
 *   /odm/etc/sensors/hwdb.d/<name>.hwdb
 *   /vendor/etc/sensors/hwdb.d/<name>.hwdb
 *   /odm/etc/hwdb.d/60-sensor.hwdb      (legacy location)
 *   /vendor/etc/hwdb.d/60-sensor.hwdb   (legacy location)
 *
 * Lookups are performed with the following match strings, built like the
 * IMPORT{builtin}="hwdb ..." rules of systemd's 60-sensor.rules:
 *   sensor:<label>:modalias:<modalias>:<dmi modalias>    (only with a label)
 *   sensor:modalias:<modalias>:<dmi modalias>
 *
 * The DMI part is always appended, so the string ends with ':' on machines
 * without DMI; device tree entries rely on this, their patterns end in ':*'.
 * The modalias, the label and the DMI modalias are sanitized the way udev
 * sanitizes "$attr{...}" substitutions, which for instance turns the device
 * tree modalias "of:NaccelerometerT(null)C..." into "of:NaccelerometerT_null_C...".
 *
 * Unlike systemd, which stops after the label match string, a device with a
 * label also falls back to the match string without it, so that generic
 * entries still apply to labelled sensors. Properties of the label specific
 * entry take precedence.
 */
class SensorHwdb {
  public:
    // Well known property names.
    static constexpr const char* kAccelMountMatrix = "ACCEL_MOUNT_MATRIX";
    static constexpr const char* kAccelLocation = "ACCEL_LOCATION";
    static constexpr const char* kProximityNearLevel = "PROXIMITY_NEAR_LEVEL";

    // Loads the database from the default locations. Returns nullptr if no
    // database file could be loaded.
    static std::unique_ptr<SensorHwdb> Load();

    // Loads the database from an explicit list of files.
    static std::unique_ptr<SensorHwdb> LoadFiles(const std::vector<std::string>& paths);

    ~SensorHwdb();

    SensorHwdb(const SensorHwdb&) = delete;
    SensorHwdb& operator=(const SensorHwdb&) = delete;

    // Returns all properties matching the given sensor. `label` may be empty.
    std::map<std::string, std::string> Lookup(const std::string& modalias,
                                              const std::string& label) const;

    // Convenience accessors.
    std::optional<std::string> GetMountMatrix(const std::string& modalias,
                                              const std::string& label) const;
    std::optional<int64_t> GetProximityNearLevel(const std::string& modalias,
                                                 const std::string& label) const;

    // The DMI modalias of this machine as used in the match strings, or an
    // empty string on machines without DMI/SMBIOS (e.g. device tree based).
    const std::string& GetDmiModalias() const { return dmi_modalias_; }

    // Reads the DMI modalias from sysfs, falling back to decoding the SMBIOS
    // tables when the kernel does not provide /sys/class/dmi/id/modalias.
    static std::string ReadDmiModalias();

  private:
    SensorHwdb(std::unique_ptr<libhwdb::Hwdb> hwdb, std::string dmi_modalias);

    std::vector<std::string> BuildMatchStrings(const std::string& modalias,
                                               const std::string& label) const;

    std::unique_ptr<libhwdb::Hwdb> hwdb_;
    std::string dmi_modalias_;
};

}  // namespace aidl::android::hardware::sensors::mainline
