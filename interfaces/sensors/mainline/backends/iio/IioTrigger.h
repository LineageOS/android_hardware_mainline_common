/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

namespace aidl::android::hardware::sensors::mainline {

/*
 * Trigger assignment for an IIO device using a triggered buffer.
 *
 * Resolution order:
 *  1. a trigger already assigned to the device (trigger/current_trigger),
 *  2. a trigger provided by the driver of the device itself, matched by name
 *     ("<name>-dev<N>", "<name>-trigger", "<name>-trig-*", "<name>*data*rdy*"),
 *  3. an hrtimer software trigger created through configfs
 *     (/config/iio/triggers/hrtimer/<name>), which requires
 *     CONFIG_IIO_CONFIGFS and CONFIG_IIO_HRTIMER_TRIGGER.
 *
 * Software triggers created by this HAL are removed again when the object is
 * destroyed. Stale triggers left behind by a crashed instance are cleaned up by
 * CleanupStaleTriggers().
 */
class IioTrigger {
  public:
    static constexpr const char* kHrtimerPrefix = "sensors-hal-hrtimer-";

    // Assigns a trigger to the device at `device_sysfs_path`. Returns nullptr if
    // no trigger could be assigned.
    static std::unique_ptr<IioTrigger> Assign(const std::string& device_sysfs_path, int dev_num,
                                              const std::string& device_name);

    // Removes hrtimer triggers created by previous instances of the HAL.
    static void CleanupStaleTriggers();

    // True if the device has a "trigger" directory, i.e. supports/needs a
    // trigger for its buffer.
    static bool DeviceHasTriggerInterface(const std::string& device_sysfs_path);

    ~IioTrigger();

    IioTrigger(const IioTrigger&) = delete;
    IioTrigger& operator=(const IioTrigger&) = delete;

    const std::string& GetName() const { return name_; }
    bool IsSoftwareTrigger() const { return owns_hrtimer_; }

    // Sets the frequency of an hrtimer trigger. Returns false for triggers
    // without a "sampling_frequency" attribute.
    bool SetFrequency(double hz);
    bool SupportsFrequency() const { return !frequency_attr_.empty(); }

  private:
    IioTrigger(std::string device_sysfs_path, std::string name, bool owns_hrtimer);

    static std::optional<std::string> FindTriggerSysfsDir(const std::string& trigger_name);
    static std::optional<std::string> FindDeviceTrigger(int dev_num,
                                                        const std::string& device_name);
    static std::optional<std::string> CreateHrtimerTrigger(int dev_num);
    static bool WriteCurrentTrigger(const std::string& device_sysfs_path,
                                    const std::string& trigger_name);

    const std::string device_sysfs_path_;
    const std::string name_;
    const bool owns_hrtimer_;
    std::string frequency_attr_;
};

}  // namespace aidl::android::hardware::sensors::mainline
