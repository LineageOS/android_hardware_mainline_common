/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <map>

#include <gtest/gtest.h>

#include "Properties.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

class DevicePropertiesTest : public ::testing::Test {
  protected:
    Properties::DeviceProperties Load(const std::vector<std::string>& selectors) {
        return Properties::LoadDeviceProperties(selectors, [this](const std::string& name) {
            auto it = values_.find(name);
            return it == values_.end() ? std::string() : it->second;
        });
    }

    std::map<std::string, std::string> values_;
};

TEST_F(DevicePropertiesTest, NothingSet) {
    const auto props = Load({"video0", "usb:046d:082d"});
    EXPECT_FALSE(props.enabled.has_value());
    EXPECT_FALSE(props.internal.has_value());
}

TEST_F(DevicePropertiesTest, AnySelectorMatches) {
    values_["vendor.camera.device.usb:046d:082d.internal"] = "true";
    values_["vendor.camera.device.video0.enabled"] = "0";
    const auto props = Load({"video0", "usb:046d:082d"});
    EXPECT_EQ(props.internal, true);
    EXPECT_EQ(props.enabled, false);
}

TEST_F(DevicePropertiesTest, MostSpecificSelectorWins) {
    values_["vendor.camera.device.video0.internal"] = "false";
    values_["vendor.camera.device.usb:046d:082d.internal"] = "true";
    EXPECT_EQ(Load({"video0", "usb:046d:082d"}).internal, false);
    EXPECT_EQ(Load({"usb:046d:082d", "video0"}).internal, true);
}

TEST_F(DevicePropertiesTest, InvalidValueFallsThrough) {
    values_["vendor.camera.device.video0.internal"] = "maybe";
    values_["vendor.camera.device.card.internal"] = "yes";
    EXPECT_EQ(Load({"video0", "card"}).internal, true);
}

TEST_F(DevicePropertiesTest, EmptySelectorIgnored) {
    values_["vendor.camera.device..internal"] = "true";
    EXPECT_FALSE(Load({""}).internal.has_value());
}

TEST(SanitizeSelectorTest, KeepsLegalCharacters) {
    EXPECT_EQ(Properties::SanitizeSelector("video0"), "video0");
    EXPECT_EQ(Properties::SanitizeSelector("usb:046d:082d"), "usb:046d:082d");
    EXPECT_EQ(Properties::SanitizeSelector("a-b@c"), "a-b@c");
}

TEST(SanitizeSelectorTest, ReplacesEverythingElse) {
    EXPECT_EQ(Properties::SanitizeSelector("HD Pro Webcam C920"), "HD_Pro_Webcam_C920");
    EXPECT_EQ(Properties::SanitizeSelector("usb-0000:00:14.0-6"), "usb-0000:00:14_0-6");
    EXPECT_EQ(Properties::SanitizeSelector("a..b/c"), "a__b_c");
    EXPECT_EQ(Properties::SanitizeSelector("Integrated Camera: Integrated C"),
              "Integrated_Camera:_Integrated_C");
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
