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
#include "config/CameraHwdb.h"
#include "flash/FlashLed.h"
#include "provider/MediaPipeline.h"
#include "v4l2/DeviceOpeners.h"

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
    // What the camera hwdb says about it.
    CameraHwdb::Entry hwdb;
    // Where `internal` came from, for the log.
    std::string internal_source;
    // Only meaningful for internal cameras.
    Facing facing = Facing::kBack;
    // Where `facing` came from; false when nothing determined it.
    std::string facing_source;
    bool facing_known = false;
    // ANDROID_SENSOR_ORIENTATION.
    int rotation = 0;
    // See Properties::prefer_rgb / advertise_rgb.
    bool prefer_rgb = false;
    bool advertise_rgb = false;
    // For a sensor behind a media controller: the pipeline to set up before
    // capturing from `info`. Null for plain capture nodes.
    std::shared_ptr<const MediaPipeline> pipeline;
    // Flash LEDs, as OpenFlashLed() takes them; see AssignFlashLeds().
    std::vector<std::string> flash_leds;
};

struct DiscoveryResult {
    std::vector<CameraCandidate> cameras;
    // A node could not be opened for a reason that may go away on its own,
    // e.g. ueventd did not apply its permissions yet. Scan again later.
    bool retry = false;
};

// Finds all cameras: camera sensors behind /dev/media* nodes and plain
// capture nodes (/dev/video*). `hwdb` may be null.
//
// `known` are the cameras found by an earlier scan. Their nodes are not
// probed again as long as their device number did not change.
DiscoveryResult DiscoverCameras(const Properties& properties, const CameraHwdb* hwdb,
                                const std::vector<CameraCandidate>& known,
                                const DeviceOpeners& open = DeviceOpeners(),
                                const std::string& dev_dir = "/dev",
                                const std::string& leds_dir = kLedClassDir);

// Gives cameras their flash LEDs (CameraCandidate::flash_leds):
//   - the LED class devices named by the camera's "flash_led" property;
//   - otherwise the V4L2 flash sub-devices linked to its sensor.
// When neither applies to any camera, all flash LEDs of `led_class` (see
// ListFlashLeds()) go to the first internal back facing camera, by key,
// which is the order IDs are given in.
void AssignFlashLeds(std::vector<CameraCandidate>* cameras,
                     const std::vector<std::string>& led_class);

// Builds cameras from the sensors of a media device that have a usable
// pipeline.
std::vector<CameraCandidate> ProbeMediaDevice(const Properties& properties, MediaDevice* media,
                                              const DeviceOpeners& open);

// Selectors of a sensor behind a media controller, most specific first:
//   sensor entity "ov5675_2-0036"
//   sensor model  "ov5675"
std::vector<std::string> MediaSelectors(const MediaPipeline& pipeline);

// Builds a camera from an opened capture node, or returns nullopt (and logs
// why) when the node is not usable as a camera. `hwdb` may be null.
std::optional<CameraCandidate> ProbeCaptureNode(const Properties& properties,
                                                const CameraHwdb* hwdb, VideoDevice* device);

// Selectors of a capture node, most specific first:
//   node name     "video0"
//   bus_info      "usb-0000:00:14_0-6"
//   USB ID        "usb:046d:0825"
//   card name     "HD_Pro_Webcam_C920"
std::vector<std::string> DeviceSelectors(const VideoDeviceInfo& info);

}  // namespace aidl::android::hardware::camera::mainline
