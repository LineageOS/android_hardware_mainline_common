/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

namespace aidl::android::hardware::sensors::mainline {

// Builds a "dmi:bvn...:bvr...:..." modalias string, in the same format as the
// kernel's /sys/class/dmi/id/modalias, by decoding the raw SMBIOS tables from
// /sys/firmware/dmi/tables. Returns an empty string when the tables are not
// available or cannot be parsed.
std::string BuildDmiModaliasFromSmbios();

}  // namespace aidl::android::hardware::sensors::mainline
