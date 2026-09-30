/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include "config/CameraHwdb.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

// Entries in the style of systemd's hwdb.d/70-cameras.hwdb.
constexpr char kHwdb[] = R"(
# Generic
camera:usb:v*p*:name:*IR Camera*:
 ID_INFRARED_CAMERA=1

# Chicony Electronics Co., Ltd HP Wide Vision FHD Camera (IR function)
camera:usb:v04f2pb634:name:*I:
 ID_INFRARED_CAMERA=1

camera:usb:v04f2pb612:name:*USB2.0 FHD UVC WebCam* USB2.0 F*:
 ID_CAMERA_DIRECTION=front
)";

TEST(CameraHwdbTest, MatchKey) {
    EXPECT_EQ(CameraHwdb::MatchKey(0x046d, 0x082d, "HD Pro Webcam C920"),
              "camera:usb:v046dp082d:name:HD Pro Webcam C920:");
    // Sanitized like udev's $attr{name}.
    EXPECT_EQ(CameraHwdb::MatchKey(0x04f2, 0xb634, "Cam (IR)"),
              "camera:usb:v04f2pb634:name:Cam _IR_:");
}

TEST(CameraHwdbTest, Lookup) {
    const auto hwdb = CameraHwdb::FromContent(kHwdb);
    ASSERT_NE(hwdb, nullptr);

    auto entry = hwdb->Lookup(0x04f2, 0xb612, "USB2.0 FHD UVC WebCam: USB2.0 F");
    EXPECT_EQ(entry.direction, Facing::kFront);
    EXPECT_FALSE(entry.infrared);

    entry = hwdb->Lookup(0x04f2, 0xb634, "HP Wide Vision FHD Camera: HP I");
    EXPECT_TRUE(entry.infrared);
    EXPECT_FALSE(entry.direction.has_value());

    entry = hwdb->Lookup(0x1234, 0x5678, "Integrated IR Camera");
    EXPECT_TRUE(entry.infrared);

    entry = hwdb->Lookup(0x046d, 0x082d, "HD Pro Webcam C920");
    EXPECT_FALSE(entry.infrared);
    EXPECT_FALSE(entry.direction.has_value());
}

TEST(CameraHwdbTest, UnknownDirectionIgnored) {
    const auto hwdb = CameraHwdb::FromContent(
            "camera:usb:v*p*:name:*:\n"
            " ID_CAMERA_DIRECTION=sideways\n");
    ASSERT_NE(hwdb, nullptr);
    EXPECT_FALSE(hwdb->Lookup(1, 2, "x").direction.has_value());
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
