/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>
#include <system/camera_metadata.h>

#include "utils/Metadata.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

TEST(MetadataTest, SetAndGet) {
    Metadata m;
    EXPECT_TRUE(m.Empty());
    EXPECT_TRUE(m.SetU8(ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_MODE_ON));
    EXPECT_TRUE(m.SetI32(ANDROID_SENSOR_ORIENTATION, 90));
    EXPECT_TRUE(m.SetI64(ANDROID_SENSOR_TIMESTAMP, 123456789012345));
    EXPECT_TRUE(m.SetFloat(ANDROID_CONTROL_ZOOM_RATIO, 2.5f));
    EXPECT_EQ(m.GetU8(ANDROID_CONTROL_AE_MODE), ANDROID_CONTROL_AE_MODE_ON);
    EXPECT_EQ(m.GetI32(ANDROID_SENSOR_ORIENTATION), 90);
    EXPECT_EQ(m.GetI64(ANDROID_SENSOR_TIMESTAMP), 123456789012345);
    EXPECT_EQ(m.GetFloat(ANDROID_CONTROL_ZOOM_RATIO), 2.5f);
    EXPECT_EQ(m.EntryCount(), 4u);
}

TEST(MetadataTest, WrongTypeRejected) {
    Metadata m;
    EXPECT_FALSE(m.SetI32(ANDROID_CONTROL_AE_MODE, 1));
    EXPECT_FALSE(m.Has(ANDROID_CONTROL_AE_MODE));
    // Reading with the wrong type gives nothing.
    m.SetU8(ANDROID_CONTROL_AE_MODE, 1);
    EXPECT_FALSE(m.GetI32(ANDROID_CONTROL_AE_MODE).has_value());
}

TEST(MetadataTest, UpdateGrowsAndShrinks) {
    Metadata m;
    m.Set(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, std::vector<int32_t>{1});
    std::vector<int32_t> many(1000);
    for (int i = 0; i < 1000; ++i) many[i] = i;
    EXPECT_TRUE(m.Set(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, many));
    EXPECT_EQ(m.GetI32s(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS), many);
    EXPECT_TRUE(m.Set(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, std::vector<int32_t>{7, 8}));
    EXPECT_EQ(m.GetI32s(ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS), (std::vector<int32_t>{7, 8}));
    EXPECT_EQ(m.EntryCount(), 1u);
}

TEST(MetadataTest, ManyEntries) {
    Metadata m;
    // More entries than the initial capacity.
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(m.Set(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
                          std::vector<int32_t>(static_cast<size_t>(i + 1), i)));
    }
    const uint32_t tags[] = {ANDROID_SENSOR_ORIENTATION, ANDROID_JPEG_MAX_SIZE,
                             ANDROID_REQUEST_PARTIAL_RESULT_COUNT, ANDROID_SYNC_MAX_LATENCY};
    for (int round = 0; round < 20; ++round) {
        for (const uint32_t tag : tags) ASSERT_TRUE(m.SetI32(tag, round));
    }
    for (const uint32_t tag : tags) EXPECT_EQ(m.GetI32(tag), 19);
}

TEST(MetadataTest, EraseAndMerge) {
    Metadata a;
    a.SetI32(ANDROID_SENSOR_ORIENTATION, 90);
    a.SetU8(ANDROID_CONTROL_AE_MODE, ANDROID_CONTROL_AE_MODE_ON);
    Metadata b;
    b.SetI32(ANDROID_SENSOR_ORIENTATION, 270);
    b.SetFloat(ANDROID_CONTROL_ZOOM_RATIO, 1.0f);
    a.Merge(b);
    EXPECT_EQ(a.GetI32(ANDROID_SENSOR_ORIENTATION), 270);
    EXPECT_TRUE(a.Has(ANDROID_CONTROL_AE_MODE));
    EXPECT_TRUE(a.Has(ANDROID_CONTROL_ZOOM_RATIO));
    a.Erase(ANDROID_CONTROL_AE_MODE);
    EXPECT_FALSE(a.Has(ANDROID_CONTROL_AE_MODE));
    a.Erase(ANDROID_CONTROL_AE_MODE);  // Not present: no-op.
    EXPECT_EQ(a.EntryCount(), 2u);
}

TEST(MetadataTest, AidlRoundTrip) {
    Metadata m;
    m.SetI32(ANDROID_SENSOR_ORIENTATION, 180);
    m.Set(ANDROID_CONTROL_ZOOM_RATIO_RANGE, std::vector<float>{1.0f, 4.0f});
    const auto aidl = m.ToAidl();
    ASSERT_FALSE(aidl.metadata.empty());

    auto copy = Metadata::FromAidl(aidl);
    ASSERT_TRUE(copy.has_value());
    EXPECT_EQ(copy->GetI32(ANDROID_SENSOR_ORIENTATION), 180);
    EXPECT_EQ(copy->EntryCount(), 2u);

    // Copy constructor and assignment.
    Metadata second(*copy);
    Metadata third;
    third = second;
    EXPECT_EQ(third.GetI32(ANDROID_SENSOR_ORIENTATION), 180);
}

TEST(MetadataTest, FromAidlEmptyAndGarbage) {
    auto empty = Metadata::FromAidl({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->Empty());

    device::CameraMetadata garbage;
    garbage.metadata.assign(64, 0xa5);
    EXPECT_FALSE(Metadata::FromAidl(garbage).has_value());

    Metadata m;
    m.SetI32(ANDROID_SENSOR_ORIENTATION, 180);
    auto truncated = m.ToAidl();
    truncated.metadata.resize(truncated.metadata.size() / 2);
    EXPECT_FALSE(Metadata::FromAidl(truncated).has_value());
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
