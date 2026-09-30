/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Properties.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// A camera found on the system, before it gets an ID.
struct CameraCandidate {
    // Identity that stays the same across boots and replugs into the same
    // port: the sysfs path of the parent device. Falls back to bus_info.
    std::string key;
    // Identity of the capture node that is used.
    VideoDeviceInfo info;
    // Selectors for per-device properties, most specific first.
    std::vector<std::string> selectors;
    Properties::DeviceProperties properties;
    // Capture formats the HAL can use, with their sizes and intervals.
    std::vector<FormatDescription> formats;
    bool internal = false;
};

struct DiscoveryResult {
    std::vector<CameraCandidate> cameras;
    // A node could not be opened for a reason that may go away on its own,
    // e.g. ueventd did not apply its permissions yet. Scan again later.
    bool retry = false;
};

// Opens video nodes. A parameter for the unit tests.
using VideoDeviceOpener =
        std::function<::android::base::Result<std::unique_ptr<VideoDevice>>(const std::string&)>;

// Finds all cameras behind /dev/video* nodes.
//
// `known` maps node paths to cameras found by an earlier scan. A node whose
// device number did not change is not probed again.
DiscoveryResult DiscoverCameras(const Properties& properties,
                                const std::map<std::string, CameraCandidate>& known,
                                const VideoDeviceOpener& open = OpenVideoDevice,
                                const std::string& dev_dir = "/dev");

// Builds a camera from an opened capture node, or returns nullopt (and logs
// why) when the node is not usable as a camera.
std::optional<CameraCandidate> ProbeCaptureNode(const Properties& properties, VideoDevice* device);

// Selectors of a capture node, most specific first:
//   node name     "video0"
//   bus_info      "usb-0000:00:14_0-6"
//   USB ID        "usb:046d:0825"
//   card name     "HD_Pro_Webcam_C920"
std::vector<std::string> DeviceSelectors(const VideoDeviceInfo& info);

}  // namespace aidl::android::hardware::camera::mainline
