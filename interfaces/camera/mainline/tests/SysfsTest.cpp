/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/file.h>
#include <gtest/gtest.h>

#include "utils/Sysfs.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

class SysfsTest : public ::testing::Test {
  protected:
    std::string Write(const std::string& content) {
        const std::string path = std::string(dir_.path) + "/attr";
        EXPECT_TRUE(::android::base::WriteStringToFile(content, path));
        return path;
    }

    TemporaryDir dir_;
};

TEST_F(SysfsTest, StringTerminators) {
    using namespace std::string_literals;
    EXPECT_EQ(ReadSysfsString(Write("fixed\n")), "fixed");
    EXPECT_EQ(ReadSysfsString(Write("fixed\0"s)), "fixed");
    EXPECT_EQ(ReadSysfsString(Write("fixed\n\0"s)), "fixed");
    EXPECT_EQ(ReadSysfsString(Write("fixed\0\n"s)), "fixed");
    EXPECT_EQ(ReadSysfsString(Write("fixed")), "fixed");
    EXPECT_EQ(ReadSysfsString(Write("HD Webcam C270 \n")), "HD Webcam C270");
    EXPECT_EQ(ReadSysfsString(Write("")), "");
    EXPECT_EQ(ReadSysfsString(Write("\0"s)), "");
}

TEST_F(SysfsTest, MissingFile) {
    EXPECT_FALSE(ReadSysfsString(std::string(dir_.path) + "/missing").has_value());
    EXPECT_FALSE(ReadSysfsHex(std::string(dir_.path) + "/missing").has_value());
}

TEST_F(SysfsTest, Hex) {
    using namespace std::string_literals;
    EXPECT_EQ(ReadSysfsHex(Write("046d\n")), 0x046du);
    EXPECT_EQ(ReadSysfsHex(Write("046d\n\0"s)), 0x046du);
    EXPECT_EQ(ReadSysfsHex(Write("0x82d\0"s)), 0x82du);
    EXPECT_FALSE(ReadSysfsHex(Write("zz\n")).has_value());
    EXPECT_FALSE(ReadSysfsHex(Write("\n")).has_value());
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
