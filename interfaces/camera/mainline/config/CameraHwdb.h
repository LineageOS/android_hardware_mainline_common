/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Properties.h"

namespace libhwdb {
class Hwdb;
}

namespace aidl::android::hardware::camera::mainline {

// The systemd compatible camera hardware database (70-cameras.hwdb). It
// knows, for USB cameras identified by vendor / product ID and name, whether
// they face front or rear and whether they are infrared cameras.
//
// Read from, in order (later entries win):
//   /vendor/etc/camera/hwdb.d/*.hwdb
//   /odm/etc/camera/hwdb.d/*.hwdb
//   /vendor/etc/hwdb.d/70-cameras.hwdb
//   /odm/etc/hwdb.d/70-cameras.hwdb
class CameraHwdb {
  public:
    struct Entry {
        // ID_CAMERA_DIRECTION
        std::optional<Facing> direction;
        // ID_INFRARED_CAMERA
        bool infrared = false;
    };

    // Returns nullptr when there is no database file.
    static std::unique_ptr<CameraHwdb> Load();
    static std::unique_ptr<CameraHwdb> FromContent(const std::string& content);
    ~CameraHwdb();

    // Looks up a USB camera: `name` is the "name" attribute of its
    // video4linux device, as used by systemd's 70-camera.rules.
    Entry Lookup(uint16_t vendor_id, uint16_t product_id, const std::string& name) const;

    // "camera:usb:v046dp082d:name:HD Pro Webcam C920:"
    static std::string MatchKey(uint16_t vendor_id, uint16_t product_id, const std::string& name);

  private:
    explicit CameraHwdb(std::unique_ptr<libhwdb::Hwdb> hwdb);

    std::unique_ptr<libhwdb::Hwdb> hwdb_;
};

}  // namespace aidl::android::hardware::camera::mainline
