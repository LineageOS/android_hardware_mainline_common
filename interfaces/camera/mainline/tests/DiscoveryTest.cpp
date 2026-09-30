/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <map>
#include <set>

#include <android-base/file.h>
#include <gtest/gtest.h>

#include "provider/Discovery.h"
#include "tests/FakeVideoDevice.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

using ::android::base::Error;
using ::android::base::Result;

constexpr char kUsbInterface[] = "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-6/1-6:1.0";

std::unique_ptr<VideoDevice> Uvc(const std::string& name, std::vector<FormatDescription> formats,
                                 const std::string& sysfs = kUsbInterface) {
    return std::make_unique<FakeVideoDevice>(FakeVideoDevice::UvcInfo(name, sysfs),
                                             std::move(formats));
}

TEST(ProbeCaptureNodeTest, UvcCamera) {
    auto device = Uvc("video0", {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480),
                                 FakeVideoDevice::Format(V4L2_PIX_FMT_MJPEG, 1920, 1080)});
    const auto candidate = ProbeCaptureNode(Properties{}, device.get());
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->key, kUsbInterface);
    EXPECT_EQ(candidate->formats.size(), 2u);
    EXPECT_FALSE(candidate->internal);
    EXPECT_EQ(candidate->selectors,
              (std::vector<std::string>{"video0", "usb-0000:00:14_0-6", "usb:046d:082d",
                                        "HD_Pro_Webcam_C920"}));
}

TEST(ProbeCaptureNodeTest, DefaultInternal) {
    Properties properties;
    properties.default_internal = true;
    auto device = Uvc("video0", {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)});
    const auto candidate = ProbeCaptureNode(properties, device.get());
    ASSERT_TRUE(candidate.has_value());
    EXPECT_TRUE(candidate->internal);
}

TEST(ProbeCaptureNodeTest, KeyWithoutSysfs) {
    auto device = Uvc("video0", {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)}, "");
    const auto candidate = ProbeCaptureNode(Properties{}, device.get());
    ASSERT_TRUE(candidate.has_value());
    EXPECT_EQ(candidate->key, "usb-0000:00:14.0-6|HD Pro Webcam C920");
}

TEST(ProbeCaptureNodeTest, RejectsNonCaptureNodes) {
    const uint32_t rejected[] = {
            V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING,
            V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING,
            V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING,
            V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE,
            V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC,
    };
    for (const uint32_t caps : rejected) {
        auto info = FakeVideoDevice::UvcInfo("video0", kUsbInterface);
        info.device_caps = caps;
        FakeVideoDevice device(info, {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)});
        EXPECT_FALSE(ProbeCaptureNode(Properties{}, &device).has_value()) << std::hex << caps;
    }
}

TEST(ProbeCaptureNodeTest, AcceptsMultiplanar) {
    auto info = FakeVideoDevice::UvcInfo("video0", kUsbInterface);
    info.device_caps = V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_STREAMING;
    info.multiplanar = true;
    FakeVideoDevice device(info, {FakeVideoDevice::Format(V4L2_PIX_FMT_NV12, 640, 480)});
    EXPECT_TRUE(ProbeCaptureNode(Properties{}, &device).has_value());
}

TEST(ProbeCaptureNodeTest, BayerOnlyNeedsIsp) {
    auto device = Uvc("video0", {FakeVideoDevice::Format(V4L2_PIX_FMT_SGRBG10, 2592, 1944)});
    EXPECT_FALSE(ProbeCaptureNode(Properties{}, device.get()).has_value());
}

TEST(ProbeCaptureNodeTest, KeepsOnlyUsableFormats) {
    FormatDescription no_sizes;
    no_sizes.fourcc = V4L2_PIX_FMT_UYVY;
    auto device = Uvc("video0", {FakeVideoDevice::Format(V4L2_PIX_FMT_SGRBG10, 2592, 1944),
                                 FakeVideoDevice::Format(V4L2_PIX_FMT_Z16, 640, 480), no_sizes,
                                 FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)});
    const auto candidate = ProbeCaptureNode(Properties{}, device.get());
    ASSERT_TRUE(candidate.has_value());
    ASSERT_EQ(candidate->formats.size(), 1u);
    EXPECT_EQ(candidate->formats[0].fourcc, V4L2_PIX_FMT_YUYV);
}

TEST(ProbeCaptureNodeTest, UnsupportedOnly) {
    auto device = Uvc("video0", {FakeVideoDevice::Format(V4L2_PIX_FMT_Z16, 640, 480)});
    EXPECT_FALSE(ProbeCaptureNode(Properties{}, device.get()).has_value());
}

class DiscoverCamerasTest : public ::testing::Test {
  protected:
    void AddNode(const std::string& name) {
        ASSERT_TRUE(::android::base::WriteStringToFile("", dir_.path + std::string("/") + name));
    }

    // Opens the fake device registered for a node, or fails with `errors_`.
    Result<std::unique_ptr<VideoDevice>> Open(const std::string& path) {
        opened_.insert(path);
        const std::string name = path.substr(path.rfind('/') + 1);
        if (auto it = errors_.find(name); it != errors_.end()) {
            return Error(it->second) << "open " << path;
        }
        auto it = devices_.find(name);
        if (it == devices_.end()) return Error(ENODEV) << "no device " << path;
        return std::move(it->second);
    }

    DiscoveryResult Discover(const std::map<std::string, CameraCandidate>& known = {}) {
        return DiscoverCameras(
                Properties{}, known, [this](const std::string& path) { return Open(path); },
                dir_.path);
    }

    ::android::base::TemporaryDir dir_;
    std::map<std::string, std::unique_ptr<VideoDevice>> devices_;
    std::map<std::string, int> errors_;
    std::set<std::string> opened_;
};

TEST_F(DiscoverCamerasTest, Empty) {
    const auto result = Discover();
    EXPECT_TRUE(result.cameras.empty());
    EXPECT_FALSE(result.retry);
}

TEST_F(DiscoverCamerasTest, IgnoresOtherNodes) {
    AddNode("media0");
    AddNode("videofoo");
    AddNode("v4l-subdev0");
    const auto result = Discover();
    EXPECT_TRUE(result.cameras.empty());
    EXPECT_TRUE(opened_.empty());
}

TEST_F(DiscoverCamerasTest, OneCameraPerDevice) {
    // A capture card with two capture nodes on one interface, and a second
    // camera on another interface.
    AddNode("video10");
    AddNode("video2");
    AddNode("video3");
    devices_["video2"] = Uvc("video2", {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)});
    devices_["video3"] = Uvc("video3", {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)});
    devices_["video10"] = Uvc("video10", {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)},
                              "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-7/1-7:1.0");

    const auto result = Discover();
    ASSERT_EQ(result.cameras.size(), 2u);
    // Numerical node order: video2 is used, not video3.
    EXPECT_EQ(result.cameras[0].info.name, "video2");
    EXPECT_EQ(result.cameras[1].info.name, "video10");
}

TEST_F(DiscoverCamerasTest, RetriesInaccessibleNodes) {
    AddNode("video0");
    errors_["video0"] = EACCES;
    EXPECT_TRUE(Discover().retry);

    errors_["video0"] = EPERM;
    EXPECT_TRUE(Discover().retry);
}

TEST_F(DiscoverCamerasTest, DoesNotRetryOtherErrors) {
    AddNode("video0");
    AddNode("video1");
    errors_["video0"] = ENOENT;
    errors_["video1"] = ENOTTY;
    const auto result = Discover();
    EXPECT_TRUE(result.cameras.empty());
    EXPECT_FALSE(result.retry);
}

TEST_F(DiscoverCamerasTest, ReusesKnownNodes) {
    AddNode("video0");
    CameraCandidate known;
    known.key = kUsbInterface;
    known.info = FakeVideoDevice::UvcInfo("video0", kUsbInterface);
    known.info.path = dir_.path + std::string("/video0");
    known.info.rdev = 0;  // Regular files have no device number.

    const auto result = Discover({{known.info.path, known}});
    ASSERT_EQ(result.cameras.size(), 1u);
    EXPECT_EQ(result.cameras[0].key, kUsbInterface);
    EXPECT_TRUE(opened_.empty());
}

TEST_F(DiscoverCamerasTest, ProbesReplacedNodes) {
    AddNode("video0");
    devices_["video0"] = Uvc("video0", {FakeVideoDevice::Format(V4L2_PIX_FMT_YUYV, 640, 480)});
    CameraCandidate known;
    known.key = "old";
    known.info.path = dir_.path + std::string("/video0");
    known.info.rdev = 1234;

    const auto result = Discover({{known.info.path, known}});
    ASSERT_EQ(result.cameras.size(), 1u);
    EXPECT_EQ(result.cameras[0].key, kUsbInterface);
    EXPECT_EQ(opened_.size(), 1u);
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
