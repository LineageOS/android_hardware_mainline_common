/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/stat.h>

#include <android-base/file.h>
#include <gtest/gtest.h>

#include "flash/Flash.h"
#include "flash/FlashLed.h"
#include "provider/Discovery.h"
#include "tests/FakeFlashLed.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::common::TorchModeStatus;

// An LED class directory with LED devices in it.
class LedClassTest : public ::testing::Test {
  protected:
    void AddLed(const std::string& name, const std::string& max_brightness) {
        const std::string dir = std::string(dir_.path) + "/" + name;
        ASSERT_EQ(mkdir(dir.c_str(), 0755), 0);
        ASSERT_TRUE(::android::base::WriteStringToFile(max_brightness, dir + "/max_brightness"));
        ASSERT_TRUE(::android::base::WriteStringToFile("0\n", dir + "/brightness"));
    }

    std::string Brightness(const std::string& name) {
        std::string value;
        ::android::base::ReadFileToString(std::string(dir_.path) + "/" + name + "/brightness",
                                          &value);
        return value;
    }

    TemporaryDir dir_;
};

TEST_F(LedClassTest, SysfsLed) {
    AddLed("white:flash", "255\n");
    auto led = OpenSysfsFlashLed("white:flash", dir_.path);
    ASSERT_TRUE(led.ok()) << led.error().message();
    EXPECT_EQ((*led)->MaxLevel(), 255);
    ASSERT_TRUE((*led)->SetLevel(100).ok());
    EXPECT_EQ(Brightness("white:flash"), "100");
    ASSERT_TRUE((*led)->SetLevel(1000).ok());
    EXPECT_EQ(Brightness("white:flash"), "255");
    led->reset();
    EXPECT_EQ(Brightness("white:flash"), "0");
}

TEST_F(LedClassTest, SysfsLedErrors) {
    AddLed("zero:flash", "0\n");
    EXPECT_FALSE(OpenSysfsFlashLed("zero:flash", dir_.path).ok());
    EXPECT_FALSE(OpenSysfsFlashLed("missing:flash", dir_.path).ok());
    EXPECT_FALSE(OpenSysfsFlashLed("../white:flash", dir_.path).ok());
    EXPECT_FALSE(OpenSysfsFlashLed("..", dir_.path).ok());
}

TEST_F(LedClassTest, ListFlashLeds) {
    AddLed("white:flash", "255");
    AddLed("led:torch_0", "255");
    AddLed("yellow:flash", "255");
    AddLed("green:status", "1");
    AddLed("mmc0::", "1");
    EXPECT_EQ(ListFlashLeds(dir_.path),
              (std::vector<std::string>{"led:torch_0", "white:flash", "yellow:flash"}));
    EXPECT_TRUE(ListFlashLeds(std::string(dir_.path) + "/missing").empty());
}

TEST(FlashLedNameTest, Names) {
    EXPECT_TRUE(IsFlashLedName("white:flash"));
    EXPECT_TRUE(IsFlashLedName("white:flash-1"));
    EXPECT_TRUE(IsFlashLedName("led:torch_0"));
    EXPECT_TRUE(IsFlashLedName("Flashlight"));
    EXPECT_FALSE(IsFlashLedName("white:status"));
    EXPECT_FALSE(IsFlashLedName("input3::capslock"));
}

class FlashTest : public ::testing::Test {
  protected:
    void SetUp() override {
        std::vector<std::unique_ptr<FlashLed>> leds;
        leds.push_back(std::make_unique<FakeFlashLed>("white", 100, white_));
        leds.push_back(std::make_unique<FakeFlashLed>("yellow", 10, yellow_));
        flash_ = std::make_unique<Flash>(std::move(leds));
        flash_->SetTorchListener([this](TorchModeStatus status) { statuses_.push_back(status); });
    }

    std::shared_ptr<std::atomic<int32_t>> white_ = std::make_shared<std::atomic<int32_t>>(-1);
    std::shared_ptr<std::atomic<int32_t>> yellow_ = std::make_shared<std::atomic<int32_t>>(-1);
    std::unique_ptr<Flash> flash_;
    std::vector<TorchModeStatus> statuses_;
};

TEST_F(FlashTest, Torch) {
    EXPECT_EQ(flash_->max_level(), 100);
    EXPECT_EQ(flash_->torch_level(), flash_->default_level());

    EXPECT_EQ(flash_->SetTorch(true), Status::OK);
    EXPECT_EQ(white_->load(), 100);
    EXPECT_EQ(yellow_->load(), 10);
    EXPECT_EQ(flash_->SetTorchLevel(1), Status::OK);
    EXPECT_EQ(white_->load(), 1);
    // Scaled, but still on.
    EXPECT_EQ(yellow_->load(), 1);
    EXPECT_EQ(flash_->SetTorchLevel(55), Status::OK);
    EXPECT_EQ(yellow_->load(), 6);
    EXPECT_EQ(flash_->torch_level(), 55);
    EXPECT_EQ(flash_->SetTorchLevel(0), Status::ILLEGAL_ARGUMENT);
    EXPECT_EQ(flash_->SetTorchLevel(101), Status::ILLEGAL_ARGUMENT);

    EXPECT_EQ(flash_->SetTorch(false), Status::OK);
    EXPECT_EQ(white_->load(), 0);
    EXPECT_EQ(yellow_->load(), 0);
    // Off resets the level.
    EXPECT_EQ(flash_->torch_level(), flash_->default_level());

    EXPECT_EQ(statuses_, (std::vector<TorchModeStatus>{TorchModeStatus::AVAILABLE_ON,
                                                       TorchModeStatus::AVAILABLE_OFF}));
}

TEST_F(FlashTest, UnavailableWhileAcquired) {
    ASSERT_EQ(flash_->SetTorchLevel(20), Status::OK);
    flash_->Acquire();
    EXPECT_EQ(white_->load(), 0);
    EXPECT_EQ(flash_->SetTorch(true), Status::CAMERA_IN_USE);
    EXPECT_EQ(flash_->SetTorchLevel(10), Status::CAMERA_IN_USE);

    flash_->SetLit(true);
    EXPECT_EQ(white_->load(), 100);
    EXPECT_EQ(yellow_->load(), 10);
    flash_->Acquire();
    flash_->Release();
    EXPECT_EQ(white_->load(), 0);
    flash_->Release();
    flash_->SetLit(true);
    EXPECT_EQ(white_->load(), 0);

    EXPECT_EQ(statuses_, (std::vector<TorchModeStatus>{TorchModeStatus::AVAILABLE_ON,
                                                       TorchModeStatus::NOT_AVAILABLE,
                                                       TorchModeStatus::AVAILABLE_OFF}));
}

TEST_F(FlashTest, Detach) {
    ASSERT_EQ(flash_->SetTorch(true), Status::OK);
    flash_->Detach();
    EXPECT_EQ(white_->load(), 0);
    EXPECT_EQ(statuses_, std::vector<TorchModeStatus>{TorchModeStatus::AVAILABLE_ON});
}

TEST_F(LedClassTest, OpenSkipsUnusableLeds) {
    AddLed("white:flash", "15");
    auto flash = Flash::Open({"white:flash", "missing:flash"}, dir_.path);
    ASSERT_NE(flash, nullptr);
    EXPECT_EQ(flash->max_level(), 15);
    EXPECT_EQ(Flash::Open({"missing:flash"}, dir_.path), nullptr);
}

CameraCandidate Camera(const std::string& key, bool internal, Facing facing) {
    CameraCandidate candidate;
    candidate.key = key;
    candidate.internal = internal;
    candidate.facing = facing;
    return candidate;
}

TEST(AssignFlashLedsTest, FirstBackCamera) {
    std::vector<CameraCandidate> cameras = {
            Camera("c", true, Facing::kBack), Camera("a", true, Facing::kFront),
            Camera("b", true, Facing::kBack), Camera("0", false, Facing::kBack)};
    AssignFlashLeds(&cameras, {"white:flash", "yellow:flash"});
    EXPECT_TRUE(cameras[0].flash_leds.empty());
    EXPECT_TRUE(cameras[1].flash_leds.empty());
    EXPECT_EQ(cameras[2].flash_leds, (std::vector<std::string>{"white:flash", "yellow:flash"}));
    EXPECT_TRUE(cameras[3].flash_leds.empty());
}

TEST(AssignFlashLedsTest, NoBackCamera) {
    std::vector<CameraCandidate> cameras = {Camera("a", true, Facing::kFront),
                                            Camera("b", false, Facing::kBack)};
    AssignFlashLeds(&cameras, {"white:flash"});
    EXPECT_TRUE(cameras[0].flash_leds.empty());
    EXPECT_TRUE(cameras[1].flash_leds.empty());
}

TEST(AssignFlashLedsTest, PropertyWins) {
    std::vector<CameraCandidate> cameras = {Camera("a", true, Facing::kBack),
                                            Camera("b", true, Facing::kFront)};
    cameras[1].properties.flash_led = std::vector<std::string>{"front:flash"};
    AssignFlashLeds(&cameras, {"front:flash", "white:flash"});
    // Configured: nothing is given out automatically.
    EXPECT_TRUE(cameras[0].flash_leds.empty());
    EXPECT_EQ(cameras[1].flash_leds, std::vector<std::string>{"front:flash"});

    // "none"
    cameras[1].properties.flash_led = std::vector<std::string>{};
    AssignFlashLeds(&cameras, {"white:flash"});
    EXPECT_TRUE(cameras[0].flash_leds.empty());
    EXPECT_TRUE(cameras[1].flash_leds.empty());
}

TEST(AssignFlashLedsTest, MediaFlash) {
    std::vector<CameraCandidate> cameras = {Camera("a", true, Facing::kBack),
                                            Camera("b", true, Facing::kBack)};
    auto pipeline = std::make_shared<MediaPipeline>();
    pipeline->flash_subdevs = {"/dev/v4l-subdev3"};
    cameras[1].pipeline = pipeline;
    AssignFlashLeds(&cameras, {"white:flash"});
    EXPECT_TRUE(cameras[0].flash_leds.empty());
    EXPECT_EQ(cameras[1].flash_leds, std::vector<std::string>{"/dev/v4l-subdev3"});
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
