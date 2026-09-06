/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

class SensorHwdb {
  public:
    ~SensorHwdb();

    SensorHwdb(const SensorHwdb&) = delete;
    SensorHwdb& operator=(const SensorHwdb&) = delete;

    static std::unique_ptr<SensorHwdb> Create();

    bool GetMountMatrix(const std::string& device_modalias, const std::string& label,
                        float matrix[9]) const;

    int GetProximityNearLevel(const std::string& device_modalias, const std::string& label,
                              int default_value) const;

    std::map<std::string, std::string> GetSensorProperties(const std::string& device_modalias,
                                                           const std::string& label) const;

    static std::string GetDmiModalias();

  private:
    SensorHwdb();

    class HwdbWrapper;
    std::unique_ptr<HwdbWrapper> hwdb_;

    std::vector<std::string> BuildQueryStrings(const std::string& device_modalias,
                                               const std::string& label) const;

    bool ParseMountMatrixFromString(const std::string& content, float matrix[9]) const;
};

}  // namespace aidl::android::hardware::sensors::mainline
