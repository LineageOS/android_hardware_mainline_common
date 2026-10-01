/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <linux/videodev2.h>

#include <algorithm>
#include <set>
#include <tuple>

#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/PixelFormat.h>
#include <gtest/gtest.h>
#include <system/camera_metadata.h>

#include "device/CameraDescription.h"
#include "tests/FakeVideoDevice.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

using ::aidl::android::hardware::graphics::common::Dataspace;
using ::aidl::android::hardware::graphics::common::PixelFormat;
using device::Stream;
using device::StreamConfiguration;
using device::StreamConfigurationMode;
using device::StreamRotation;
using device::StreamType;

CameraCandidate Webcam(bool internal = false, Facing facing = Facing::kBack, int rotation = 0) {
    CameraCandidate candidate;
    candidate.key = "/sys/devices/test";
    candidate.info = FakeVideoDevice::UvcInfo("video0", candidate.key);
    FormatDescription yuyv;
    yuyv.fourcc = V4L2_PIX_FMT_YUYV;
    yuyv.sizes = {{1920, 1080, {{1, 5}}}, {640, 480, {{1, 30}}}};
    FormatDescription mjpeg;
    mjpeg.fourcc = V4L2_PIX_FMT_MJPEG;
    mjpeg.sizes = {{1920, 1080, {{1, 30}}}, {1280, 720, {{1, 30}}}};
    candidate.formats = {yuyv, mjpeg};
    candidate.internal = internal;
    candidate.facing = facing;
    candidate.rotation = rotation;
    return candidate;
}

Stream MakeStream(int id, int width, int height, PixelFormat format,
                  Dataspace dataspace = Dataspace::UNKNOWN) {
    Stream stream;
    stream.id = id;
    stream.streamType = StreamType::OUTPUT;
    stream.width = width;
    stream.height = height;
    stream.format = format;
    stream.dataSpace = dataspace;
    stream.rotation = StreamRotation::ROTATION_0;
    stream.groupId = -1;
    stream.colorSpace = -1;
    return stream;
}

StreamConfiguration Config(std::vector<Stream> streams) {
    StreamConfiguration config;
    config.streams = std::move(streams);
    config.operationMode = StreamConfigurationMode::NORMAL_MODE;
    return config;
}

TEST(CameraDescriptionTest, ExternalCharacteristics) {
    auto description = CameraDescription::Create(Webcam());
    ASSERT_NE(description, nullptr);
    const Metadata& m = description->characteristics();
    EXPECT_EQ(m.GetU8(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL),
              ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL);
    EXPECT_EQ(m.GetU8(ANDROID_LENS_FACING), ANDROID_LENS_FACING_EXTERNAL);
    EXPECT_EQ(m.GetI32(ANDROID_SENSOR_ORIENTATION), 0);
    EXPECT_EQ(m.GetI32s(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE),
              (std::vector<int32_t>{0, 0, 1920, 1080}));
}

TEST(CameraDescriptionTest, InternalCharacteristics) {
    auto description = CameraDescription::Create(Webcam(true, Facing::kFront, 270));
    ASSERT_NE(description, nullptr);
    const Metadata& m = description->characteristics();
    EXPECT_EQ(m.GetU8(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL),
              ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED);
    EXPECT_EQ(m.GetU8(ANDROID_LENS_FACING), ANDROID_LENS_FACING_FRONT);
    EXPECT_EQ(m.GetI32(ANDROID_SENSOR_ORIENTATION), 270);

    auto back = CameraDescription::Create(Webcam(true, Facing::kBack, 90));
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(back->characteristics().GetU8(ANDROID_LENS_FACING), ANDROID_LENS_FACING_BACK);
}

std::vector<uint8_t> U8s(const Metadata& m, uint32_t tag) {
    const auto entry = m.Find(tag);
    if (!entry.has_value()) return {};
    return std::vector<uint8_t>(entry->data.u8, entry->data.u8 + entry->count);
}

TEST(CameraDescriptionTest, Flash) {
    auto none = CameraDescription::Create(Webcam(true, Facing::kBack, 90));
    ASSERT_NE(none, nullptr);
    EXPECT_FALSE(none->has_flash());
    EXPECT_EQ(none->characteristics().GetU8(ANDROID_FLASH_INFO_AVAILABLE),
              ANDROID_FLASH_INFO_AVAILABLE_FALSE);
    EXPECT_EQ(U8s(none->characteristics(), ANDROID_CONTROL_AE_AVAILABLE_MODES),
              std::vector<uint8_t>{ANDROID_CONTROL_AE_MODE_ON});

    auto flash = CameraDescription::Create(Webcam(true, Facing::kBack, 90), 40);
    ASSERT_NE(flash, nullptr);
    const Metadata& m = flash->characteristics();
    EXPECT_EQ(m.GetU8(ANDROID_FLASH_INFO_AVAILABLE), ANDROID_FLASH_INFO_AVAILABLE_TRUE);
    EXPECT_EQ(m.GetI32(ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL), 40);
    EXPECT_EQ(m.GetI32(ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL), 40);
    EXPECT_EQ(
            U8s(m, ANDROID_CONTROL_AE_AVAILABLE_MODES),
            (std::vector<uint8_t>{ANDROID_CONTROL_AE_MODE_ON, ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH,
                                  ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH}));

    // A single level: no strength control.
    auto single = CameraDescription::Create(Webcam(true, Facing::kBack, 90), 1);
    ASSERT_NE(single, nullptr);
    EXPECT_FALSE(single->characteristics()
                         .GetI32(ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL)
                         .has_value());
}

TEST(CameraDescriptionTest, CharacteristicsKeysListEveryKey) {
    auto description = CameraDescription::Create(Webcam());
    ASSERT_NE(description, nullptr);
    const Metadata& m = description->characteristics();
    const auto listed = m.GetI32s(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    const std::set<int32_t> keys(listed.begin(), listed.end());
    for (const int32_t tag : m.Tags()) {
        EXPECT_EQ(keys.count(tag), 1u) << get_camera_metadata_tag_name(tag);
    }
    EXPECT_EQ(keys.size(), m.EntryCount());
}

TEST(CameraDescriptionTest, StreamConfigurationsHaveDurations) {
    auto description = CameraDescription::Create(Webcam());
    ASSERT_NE(description, nullptr);
    const Metadata& m = description->characteristics();

    const auto configs = m.GetI32s(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    ASSERT_EQ(configs.size() % 4, 0u);
    ASSERT_FALSE(configs.empty());
    auto durations = m.Find(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS);
    auto stalls = m.Find(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS);
    ASSERT_TRUE(durations.has_value());
    ASSERT_TRUE(stalls.has_value());

    std::set<std::tuple<int64_t, int64_t, int64_t>> with_duration;
    for (size_t i = 0; i + 3 < durations->count; i += 4) {
        EXPECT_GT(durations->data.i64[i + 3], 0);
        with_duration.insert(
                {durations->data.i64[i], durations->data.i64[i + 1], durations->data.i64[i + 2]});
    }
    std::set<int32_t> formats;
    for (size_t i = 0; i < configs.size(); i += 4) {
        formats.insert(configs[i]);
        EXPECT_EQ(configs[i + 3], ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
        EXPECT_EQ(with_duration.count({configs[i], configs[i + 1], configs[i + 2]}), 1u);
    }
    EXPECT_EQ(formats, (std::set<int32_t>{static_cast<int32_t>(PixelFormat::BLOB),
                                          static_cast<int32_t>(PixelFormat::IMPLEMENTATION_DEFINED),
                                          static_cast<int32_t>(PixelFormat::YCBCR_420_888)}));
    EXPECT_EQ(stalls->count, durations->count);
}

TEST(CameraDescriptionTest, FpsRanges) {
    auto description = CameraDescription::Create(Webcam());
    ASSERT_NE(description, nullptr);
    const auto& ranges = description->fps_ranges();
    auto has = [&](int32_t min, int32_t max) {
        return std::find(ranges.begin(), ranges.end(), std::array<int32_t, 2>{min, max}) !=
               ranges.end();
    };
    EXPECT_TRUE(has(30, 30));
    EXPECT_TRUE(has(15, 30));
    EXPECT_TRUE(has(5, 5));
    EXPECT_FALSE(has(15, 5));
    EXPECT_EQ(description->characteristics().GetI64(ANDROID_SENSOR_INFO_MAX_FRAME_DURATION),
              200'000'000);
}

TEST(CameraDescriptionTest, NoUsableSize) {
    CameraCandidate candidate = Webcam();
    candidate.formats.clear();
    EXPECT_EQ(CameraDescription::Create(candidate), nullptr);
}

TEST(PlanStreamsTest, Supported) {
    auto description = CameraDescription::Create(Webcam());
    ASSERT_NE(description, nullptr);
    std::string why;

    auto mode = description->PlanStreams(
            Config({MakeStream(0, 1280, 720, PixelFormat::IMPLEMENTATION_DEFINED),
                    MakeStream(1, 640, 480, PixelFormat::YCBCR_420_888),
                    MakeStream(2, 1920, 1080, PixelFormat::BLOB, Dataspace::JFIF)}),
            &why);
    ASSERT_TRUE(mode.has_value()) << why;
    EXPECT_EQ(mode->size, (Size{1920, 1080}));

    // A standard size that is no capture size.
    mode = description->PlanStreams(Config({MakeStream(0, 320, 240, PixelFormat::YCBCR_420_888)}),
                                    &why);
    ASSERT_TRUE(mode.has_value()) << why;
    EXPECT_EQ(mode->size, (Size{640, 480}));
}

TEST(PlanStreamsTest, Rejected) {
    auto description = CameraDescription::Create(Webcam());
    ASSERT_NE(description, nullptr);
    std::string why;

    auto reject = [&](StreamConfiguration config) {
        why.clear();
        EXPECT_FALSE(description->PlanStreams(config, &why).has_value());
        EXPECT_FALSE(why.empty());
    };

    reject(Config({}));
    reject(Config({MakeStream(0, 640, 480, PixelFormat::YCBCR_420_888),
                   MakeStream(1, 640, 480, PixelFormat::YCBCR_420_888),
                   MakeStream(2, 640, 480, PixelFormat::IMPLEMENTATION_DEFINED)}));
    reject(Config({MakeStream(0, 640, 480, PixelFormat::BLOB),
                   MakeStream(1, 640, 480, PixelFormat::BLOB)}));
    reject(Config({MakeStream(0, 800, 600, PixelFormat::YCBCR_420_888)}));
    reject(Config({MakeStream(0, 640, 480, PixelFormat::RGBA_8888)}));
    reject(Config({MakeStream(0, 640, 480, PixelFormat::BLOB, Dataspace::DEPTH)}));
    reject(Config({MakeStream(0, 640, 480, PixelFormat::BLOB, Dataspace::HEIF)}));

    auto rotated = MakeStream(0, 640, 480, PixelFormat::YCBCR_420_888);
    rotated.rotation = StreamRotation::ROTATION_90;
    reject(Config({rotated}));

    auto input = MakeStream(0, 640, 480, PixelFormat::YCBCR_420_888);
    input.streamType = StreamType::INPUT;
    reject(Config({input}));

    auto physical = MakeStream(0, 640, 480, PixelFormat::YCBCR_420_888);
    physical.physicalCameraId = "1";
    reject(Config({physical}));

    auto high_speed = Config({MakeStream(0, 640, 480, PixelFormat::YCBCR_420_888)});
    high_speed.operationMode = StreamConfigurationMode::CONSTRAINED_HIGH_SPEED_MODE;
    reject(high_speed);
}

TEST(PlanStreamsTest, AdvertisedRgb) {
    CameraCandidate candidate = Webcam();
    candidate.advertise_rgb = true;
    auto description = CameraDescription::Create(candidate);
    ASSERT_NE(description, nullptr);
    std::string why;
    EXPECT_TRUE(
            description
                    ->PlanStreams(Config({MakeStream(0, 640, 480, PixelFormat::RGBA_8888)}), &why)
                    .has_value())
            << why;
    const auto configs =
            description->characteristics().GetI32s(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS);
    bool found = false;
    for (size_t i = 0; i < configs.size(); i += 4) {
        found |= configs[i] == static_cast<int32_t>(PixelFormat::RGBA_8888);
    }
    EXPECT_TRUE(found);
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
