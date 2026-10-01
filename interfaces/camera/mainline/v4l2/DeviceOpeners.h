/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "v4l2/MediaDevice.h"
#include "v4l2/SubDevice.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// How device nodes are opened; the unit tests substitute fakes.
struct DeviceOpeners {
    VideoDeviceOpener video = OpenVideoDevice;
    MediaDeviceOpener media = [](const std::string& path) { return OpenMediaDevice(path); };
    SubDeviceOpener subdev = OpenSubDevice;
};

}  // namespace aidl::android::hardware::camera::mainline
