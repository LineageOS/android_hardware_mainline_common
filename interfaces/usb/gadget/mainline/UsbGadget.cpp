/*
 * SPDX-FileCopyrightText: 2021 The Android Open Source Project
 * SPDX-FileCopyrightText: 2024 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.usb.gadget.aidl-service"

#include "UsbGadget.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <android-base/properties.h>

#include <aidl/android/frameworks/stats/IStats.h>

namespace aidl {
namespace android {
namespace hardware {
namespace usb {
namespace gadget {

using ::android::base::GetBoolProperty;
using ::android::base::GetProperty;
using ::android::hardware::google::pixel::usb::kUvcEnabled;

UsbGadget::UsbGadget() {
    if (kGadgetName.empty()) {
        ALOGE("USB controller name not set");
        abort();
    }
    if (access(OS_DESC_PATH, R_OK) != 0) {
        ALOGE("configfs setup not done yet");
        abort();
    }

    mMonitorFfs = new MonitorFfs(kGadgetName.c_str());
}

static inline std::string getUdcNodeHelper(const std::string path) {
    return UDC_PATH + kGadgetName + "/" + path;
}

void currentFunctionsAppliedCallback(bool functionsApplied, void* payload) {
    UsbGadget* gadget = (UsbGadget*)payload;
    gadget->mCurrentUsbFunctionsApplied = functionsApplied;
}

ScopedAStatus UsbGadget::getCurrentUsbFunctions(const shared_ptr<IUsbGadgetCallback>& callback,
                                                int64_t in_transactionId) {
    ScopedAStatus ret = callback->getCurrentUsbFunctionsCb(
            mCurrentUsbFunctions,
            mCurrentUsbFunctionsApplied ? Status::FUNCTIONS_APPLIED : Status::FUNCTIONS_NOT_APPLIED,
            in_transactionId);
    if (!ret.isOk())
        ALOGE("Call to getCurrentUsbFunctionsCb failed %s", ret.getDescription().c_str());

    return ScopedAStatus::ok();
}

ScopedAStatus UsbGadget::getUsbSpeed(const shared_ptr<IUsbGadgetCallback>& callback,
                                     int64_t in_transactionId) {
    std::string current_speed;
    if (ReadFileToString(getUdcNodeHelper(SPEED_PATH), &current_speed)) {
        current_speed = Trim(current_speed);
        ALOGI("current USB speed is %s", current_speed.c_str());
        if (current_speed == "low-speed")
            mUsbSpeed = UsbSpeed::LOWSPEED;
        else if (current_speed == "full-speed")
            mUsbSpeed = UsbSpeed::FULLSPEED;
        else if (current_speed == "high-speed")
            mUsbSpeed = UsbSpeed::HIGHSPEED;
        else if (current_speed == "super-speed")
            mUsbSpeed = UsbSpeed::SUPERSPEED;
        else if (current_speed == "super-speed-plus")
            mUsbSpeed = UsbSpeed::SUPERSPEED_10Gb;
        else if (current_speed == "UNKNOWN")
            mUsbSpeed = UsbSpeed::UNKNOWN;
        else
            mUsbSpeed = UsbSpeed::UNKNOWN;
    } else {
        ALOGE("Fail to read current speed");
        mUsbSpeed = UsbSpeed::UNKNOWN;
    }

    if (callback) {
        ScopedAStatus ret = callback->getUsbSpeedCb(mUsbSpeed, in_transactionId);

        if (!ret.isOk()) ALOGE("Call to getUsbSpeedCb failed %s", ret.getDescription().c_str());
    }

    return ScopedAStatus::ok();
}

Status UsbGadget::tearDownGadget() {
    if (Status(resetGadget()) != Status::SUCCESS) {
        return Status::ERROR;
    }

    if (mMonitorFfs->isMonitorRunning()) {
        mMonitorFfs->reset();
    } else {
        ALOGI("mMonitor not running");
    }
    return Status::SUCCESS;
}

static Status validateAndSetVidPid(int64_t functions) {
    Status ret;
    std::string vid, pid;

    vid = "0x" + GetProperty("ro.vendor.usb.vid", "18d1");

    switch (functions) {
        case GadgetFunction::MTP:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.mtp", "4ee1");
            break;
        case GadgetFunction::ADB | GadgetFunction::MTP:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.mtp.adb", "4ee2");
            break;
        case GadgetFunction::RNDIS:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.rndis", "4ee3");
            break;
        case GadgetFunction::ADB | GadgetFunction::RNDIS:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.rndis.adb", "4ee4");
            break;
        case GadgetFunction::PTP:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.ptp", "4ee5");
            break;
        case GadgetFunction::ADB | GadgetFunction::PTP:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.ptp.adb", "4ee6");
            break;
        case GadgetFunction::ADB:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.adb", "4ee7");
            break;
        case GadgetFunction::MIDI:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.midi", "4ee8");
            break;
        case GadgetFunction::ADB | GadgetFunction::MIDI:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.midi.adb", "4ee9");
            break;
        case GadgetFunction::ACCESSORY:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.accessory", "2d00");
            break;
        case GadgetFunction::ADB | GadgetFunction::ACCESSORY:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.accessory.adb", "2d01");
            break;
        case GadgetFunction::AUDIO_SOURCE:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.audio_source", "2d02");
            break;
        case GadgetFunction::ADB | GadgetFunction::AUDIO_SOURCE:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.audio_source.adb", "2d03");
            break;
        case GadgetFunction::ACCESSORY | GadgetFunction::AUDIO_SOURCE:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.accessory.audio_source", "2d04");
            break;
        case GadgetFunction::ADB | GadgetFunction::ACCESSORY | GadgetFunction::AUDIO_SOURCE:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.accessory.audio_source.adb", "2d05");
            break;
        case GadgetFunction::NCM:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.ncm", "4eeb");
            break;
        case GadgetFunction::ADB | GadgetFunction::NCM:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.ncm.adb", "4eec");
            break;
        case GadgetFunction::UVC:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.uvc", "4eed");
            break;
        case GadgetFunction::ADB | GadgetFunction::UVC:
            pid = "0x" + GetProperty("ro.vendor.usb.pid.uvc.adb", "4eee");
            break;
        default:
            ALOGE("Combination not supported");
            ret = Status::CONFIGURATION_NOT_SUPPORTED;
            goto error;
    }

    ret = Status(setVidPid(vid.c_str(), pid.c_str()));
    if (ret != Status::SUCCESS) {
        ALOGE("Failed to update vid/pid");
        goto error;
    }
error:
    return ret;
}

ScopedAStatus UsbGadget::reset(const shared_ptr<IUsbGadgetCallback>& callback,
                               int64_t in_transactionId) {
    ALOGI("USB Gadget reset");

    if (!WriteStringToFile("none", PULLUP_PATH)) {
        ALOGI("Gadget cannot be pulled down");
        if (callback) callback->resetCb(Status::ERROR, in_transactionId);
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(-1,
                                                                  "Gadget cannot be pulled down");
    }

    usleep(kDisconnectWaitUs);

    if (!WriteStringToFile(kGadgetName, PULLUP_PATH)) {
        ALOGI("Gadget cannot be pulled up");
        if (callback) callback->resetCb(Status::ERROR, in_transactionId);
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(-1, "Gadget cannot be pulled up");
    }
    if (callback) callback->resetCb(Status::SUCCESS, in_transactionId);

    return ScopedAStatus::ok();
}

Status UsbGadget::setupFunctions(int64_t functions, const shared_ptr<IUsbGadgetCallback>& callback,
                                 uint64_t timeout, int64_t in_transactionId) {
    bool ffsEnabled = false;
    int i = 0;

    if (Status(addGenericAndroidFunctions(mMonitorFfs, functions, &ffsEnabled, &i)) !=
        Status::SUCCESS)
        return Status::ERROR;

    if ((functions & GadgetFunction::ADB) != 0) {
        ffsEnabled = true;
        if (Status(addAdb(mMonitorFfs, &i)) != Status::SUCCESS) return Status::ERROR;
    }

    if ((functions & GadgetFunction::NCM) != 0) {
        ALOGI("setCurrentUsbFunctions ncm");
        if (linkFunction("ncm.gs9", i++)) return Status::ERROR;
    }

    // Pull up the gadget right away when there are no ffs functions.
    if (!ffsEnabled) {
        if (!WriteStringToFile(kGadgetName, PULLUP_PATH)) return Status::ERROR;
        mCurrentUsbFunctionsApplied = true;
        if (callback)
            callback->setCurrentUsbFunctionsCb(functions, Status::SUCCESS, in_transactionId);
        return Status::SUCCESS;
    }

    mMonitorFfs->registerFunctionsAppliedCallback(&currentFunctionsAppliedCallback, this);
    // Monitors the ffs paths to pull up the gadget when descriptors are written.
    // Also takes of the pulling up the gadget again if the userspace process
    // dies and restarts.
    mMonitorFfs->startMonitor();

    if (kDebug) ALOGI("Mainthread in Cv");

    if (callback) {
        bool pullup = mMonitorFfs->waitForPullUp(timeout);
        ScopedAStatus ret = callback->setCurrentUsbFunctionsCb(
                functions, pullup ? Status::SUCCESS : Status::ERROR, in_transactionId);
        if (!ret.isOk()) {
            ALOGE("setCurrentUsbFunctionsCb error %s", ret.getDescription().c_str());
            return Status::ERROR;
        }
    }
    return Status::SUCCESS;
}

ScopedAStatus UsbGadget::setCurrentUsbFunctions(int64_t functions,
                                                const shared_ptr<IUsbGadgetCallback>& callback,
                                                int64_t timeout, int64_t in_transactionId) {
    std::unique_lock<std::mutex> lk(mLockSetCurrentFunction);
    std::string current_usb_power_operation_mode, current_usb_type;
    std::string usb_limit_sink_enable;

    mCurrentUsbFunctions = functions;
    mCurrentUsbFunctionsApplied = false;

    // Unlink the gadget and stop the monitor if running.
    Status status = tearDownGadget();
    if (status != Status::SUCCESS) {
        goto error;
    }

    ALOGI("Returned from tearDown gadget");

    // Leave the gadget pulled down to give time for the host to sense disconnect.
    usleep(kDisconnectWaitUs);

    if (functions == GadgetFunction::NONE) {
        if (callback == NULL)
            return ScopedAStatus::fromServiceSpecificErrorWithMessage(-1, "callback == NULL");
        ScopedAStatus ret = callback->setCurrentUsbFunctionsCb(functions, status, in_transactionId);
        if (!ret.isOk())
            ALOGE("Error while calling setCurrentUsbFunctionsCb %s", ret.getDescription().c_str());
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                -1, "Error while calling setCurrentUsbFunctionsCb");
    }

    status = validateAndSetVidPid(functions);

    if (status != Status::SUCCESS) {
        goto error;
    }

    status = setupFunctions(functions, callback, timeout, in_transactionId);
    if (status != Status::SUCCESS) {
        goto error;
    }

    ALOGI("Usb Gadget setcurrent functions called successfully");
    return ScopedAStatus::ok();

error:
    ALOGI("Usb Gadget setcurrent functions failed");
    if (callback == NULL)
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(
                -1, "Usb Gadget setcurrent functions failed");
    ScopedAStatus ret = callback->setCurrentUsbFunctionsCb(functions, status, in_transactionId);
    if (!ret.isOk())
        ALOGE("Error while calling setCurrentUsbFunctionsCb %s", ret.getDescription().c_str());
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(
            -1, "Error while calling setCurrentUsbFunctionsCb");
}
}  // namespace gadget
}  // namespace usb
}  // namespace hardware
}  // namespace android
}  // namespace aidl
