/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include <gtest/gtest.h>

#include "provider/CameraIdAllocator.h"

namespace aidl::android::hardware::camera::mainline {
namespace {

TEST(CameraIdAllocatorTest, SeparateRanges) {
    CameraIdAllocator ids(100);
    EXPECT_EQ(ids.Allocate("a", true), 0);
    EXPECT_EQ(ids.Allocate("b", true), 1);
    EXPECT_EQ(ids.Allocate("c", false), 100);
    EXPECT_EQ(ids.Allocate("d", false), 101);
}

TEST(CameraIdAllocatorTest, ReplugKeepsId) {
    CameraIdAllocator ids(100);
    EXPECT_EQ(ids.Allocate("a", false), 100);
    EXPECT_EQ(ids.Allocate("b", false), 101);
    ids.Release("a");
    EXPECT_EQ(ids.Allocate("a", false), 100);
}

TEST(CameraIdAllocatorTest, FreedIdReusedByOthers) {
    CameraIdAllocator ids(100);
    EXPECT_EQ(ids.Allocate("a", false), 100);
    ids.Release("a");
    EXPECT_EQ(ids.Allocate("b", false), 100);
    // "a" comes back, its old ID is taken.
    EXPECT_EQ(ids.Allocate("a", false), 101);
}

TEST(CameraIdAllocatorTest, RangeChangeGetsNewId) {
    CameraIdAllocator ids(100);
    EXPECT_EQ(ids.Allocate("a", false), 100);
    ids.Release("a");
    EXPECT_EQ(ids.Allocate("a", true), 0);
}

TEST(CameraIdAllocatorTest, ReleaseUnknownIsHarmless) {
    CameraIdAllocator ids(100);
    ids.Release("unknown");
    EXPECT_EQ(ids.Allocate("a", true), 0);
}

TEST(CameraIdAllocatorTest, InternalOverflowStaysUnique) {
    CameraIdAllocator ids(2);
    EXPECT_EQ(ids.Allocate("e", false), 2);
    EXPECT_EQ(ids.Allocate("a", true), 0);
    EXPECT_EQ(ids.Allocate("b", true), 1);
    EXPECT_EQ(ids.Allocate("c", true), 3);
}

}  // namespace
}  // namespace aidl::android::hardware::camera::mainline
