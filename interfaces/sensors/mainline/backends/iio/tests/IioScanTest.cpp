/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "IioScan.h"

#include <gtest/gtest.h>

namespace aidl::android::hardware::sensors::mainline::iio {
namespace {

TEST(IioScanTest, ParsesRepeatAndRejectsInvalidTypes) {
    ScanType type;
    ASSERT_TRUE(ParseScanType("be:s12/16X3>>4", &type));
    EXPECT_TRUE(type.big_endian);
    EXPECT_TRUE(type.is_signed);
    EXPECT_EQ(type.real_bits, 12);
    EXPECT_EQ(type.storage_bits, 16);
    EXPECT_EQ(type.repeat, 3);
    EXPECT_EQ(type.shift, 4);
    EXPECT_FALSE(ParseScanType("le:s17/16>>0", &type));
    EXPECT_FALSE(ParseScanType("le:u8/12>>0", &type));
}

TEST(IioScanTest, LayoutMatchesKernelAlignmentAndFinalPadding) {
    std::vector<ScanElement> elements = {
            {.index = 3, .type = {.storage_bits = 64}},
            {.index = 0, .type = {.storage_bits = 16, .repeat = 3}},
            {.index = 2, .type = {.storage_bits = 32}},
    };
    auto stride = ComputeScanLayout(&elements);
    ASSERT_TRUE(stride.has_value());
    ASSERT_EQ(elements.size(), 3u);
    EXPECT_EQ(elements[0].offset, 0u);
    EXPECT_EQ(elements[1].offset, 8u);
    EXPECT_EQ(elements[2].offset, 16u);
    EXPECT_EQ(*stride, 24u);
}

TEST(IioScanTest, LayoutAppendsTimestampAfterDataChannels) {
    std::vector<ScanElement> elements = {
            {.index = 0, .type = {.storage_bits = 64}, .is_timestamp = true},
            {.index = 1, .type = {.storage_bits = 16}},
    };
    auto stride = ComputeScanLayout(&elements);
    ASSERT_TRUE(stride.has_value());
    EXPECT_EQ(elements[0].index, 1);
    EXPECT_EQ(elements[0].offset, 0u);
    EXPECT_EQ(elements[1].index, 0);
    EXPECT_EQ(elements[1].offset, 8u);
    EXPECT_EQ(*stride, 16u);
}

TEST(IioScanTest, DecodesEndianShiftSignAndFullWidth) {
    const uint8_t little[] = {0xf0, 0xff};
    ScanType signed12 = {.is_signed = true, .real_bits = 12, .storage_bits = 16, .shift = 4};
    auto negative = DecodeScanValue(little, sizeof(little), signed12);
    ASSERT_TRUE(negative.has_value());
    EXPECT_EQ(negative->AsDouble(), -1.0);

    const uint8_t big[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    ScanType unsigned64 = {.big_endian = true, .real_bits = 64, .storage_bits = 64};
    auto full = DecodeScanValue(big, sizeof(big), unsigned64);
    ASSERT_TRUE(full.has_value());
    EXPECT_EQ(full->bits, 0x0123456789abcdefULL);
}

TEST(IioScanTest, DecodesNarrowAndRepeatedElementsWithoutOverread) {
    const uint8_t data[] = {0x7f, 0x80, 0x55};
    ScanType repeated = {.is_signed = true, .real_bits = 8, .storage_bits = 8, .repeat = 2};
    auto first = DecodeScanValue(data, sizeof(data), repeated, 0);
    auto second = DecodeScanValue(data, sizeof(data), repeated, 1);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->AsDouble(), 127.0);
    EXPECT_EQ(second->AsDouble(), -128.0);
}

TEST(IioScanTest, RejectsDuplicateIndicesIncludingTimestamp) {
    std::vector<ScanElement> elements = {
            {.index = 1, .type = {.storage_bits = 16}},
            {.index = 1, .type = {.storage_bits = 64}, .is_timestamp = true},
    };
    EXPECT_FALSE(ComputeScanLayout(&elements).has_value());
}

TEST(IioScanTest, ConvertsAndroidUnits) {
    EXPECT_DOUBLE_EQ(ConvertUnit(1.0, Unit::kAcceleration), 1.0);
    EXPECT_DOUBLE_EQ(ConvertUnit(1.0, Unit::kAngularVelocity), 1.0);
    EXPECT_DOUBLE_EQ(ConvertUnit(1.0, Unit::kMagneticFieldGauss), 100.0);
    EXPECT_DOUBLE_EQ(ConvertUnit(1.0, Unit::kPressureKilopascal), 10.0);
    EXPECT_DOUBLE_EQ(ConvertUnit(1000.0, Unit::kRelativeHumidityMilliPercent), 1.0);
    EXPECT_DOUBLE_EQ(ConvertUnit(1000.0, Unit::kTemperatureMilliCelsius), 1.0);
    EXPECT_DOUBLE_EQ(ConvertUnit(1.0, Unit::kProximityMeters), 100.0);
}

}  // namespace
}  // namespace aidl::android::hardware::sensors::mainline::iio
