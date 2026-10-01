/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Discovery"

#include "provider/Discovery.h"

#include <dirent.h>
#include <linux/videodev2.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <set>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include "v4l2/PixelFormats.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

// Number of a "<prefix>N" node name, nullopt for anything else.
std::optional<unsigned> NodeNumber(const std::string& name, const std::string& prefix) {
    if (!::android::base::StartsWith(name, prefix)) return std::nullopt;
    unsigned number;
    if (!::android::base::ParseUint(name.substr(prefix.size()), &number)) return std::nullopt;
    return number;
}

// "<prefix>N" nodes in `dev_dir`, in numerical order.
std::vector<std::string> ListNodes(const std::string& dev_dir, const std::string& prefix) {
    std::vector<std::pair<unsigned, std::string>> nodes;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(dev_dir.c_str()), closedir);
    if (dir == nullptr) {
        PLOG(ERROR) << "failed to open " << dev_dir;
        return {};
    }
    while (const dirent* entry = readdir(dir.get())) {
        const std::string name = entry->d_name;
        if (auto number = NodeNumber(name, prefix); number.has_value()) {
            nodes.emplace_back(*number, dev_dir + "/" + name);
        }
    }
    std::sort(nodes.begin(), nodes.end());
    std::vector<std::string> paths;
    for (auto& [number, path] : nodes) paths.push_back(std::move(path));
    return paths;
}

bool IsTransientOpenError(int error) {
    // ueventd creates the node before it applies owner, mode and SELinux
    // label, which it does right after.
    return error == EACCES || error == EPERM;
}

std::string CameraKey(const VideoDeviceInfo& info) {
    if (!info.sysfs_device.empty()) return info.sysfs_device;
    // No sysfs (unusual): the capture node of a device is unique per bus_info
    // and card, which is the best there is.
    return info.bus_info + "|" + info.card;
}

// Decides internal / external, facing and rotation, from the most to the
// least authoritative source:
//   internal: property, firmware orientation control, USB port (built in or
//             removable), default_internal
//   facing:   property, firmware orientation control, camera hwdb, built-in
//             USB camera (front, like a laptop's), back
//   rotation: property, firmware rotation control, 0
// facing_by_resolution may still change the facing of cameras that ended up
// with the last default, see DiscoverCameras().
void ResolvePlacement(const Properties& properties, ControlDevice* device, bool sensor,
                      CameraCandidate* candidate) {
    const auto& props = candidate->properties;
    const auto& info = candidate->info;

    // Set from the device tree ("orientation", "rotation") or ACPI (_PLD) on
    // drivers that call v4l2_ctrl_new_fwnode_properties().
    std::optional<int32_t> orientation;
    if (device->HasControl(V4L2_CID_CAMERA_ORIENTATION)) {
        orientation = device->GetControl(V4L2_CID_CAMERA_ORIENTATION);
    }
    std::optional<int32_t> sensor_rotation;
    if (device->HasControl(V4L2_CID_CAMERA_SENSOR_ROTATION)) {
        sensor_rotation = device->GetControl(V4L2_CID_CAMERA_SENSOR_ROTATION);
    }

    if (props.internal.has_value()) {
        candidate->internal = *props.internal;
        candidate->internal_source = "property";
    } else if (orientation.has_value()) {
        candidate->internal = *orientation != V4L2_CAMERA_ORIENTATION_EXTERNAL;
        candidate->internal_source = "firmware";
    } else if (sensor) {
        // A sensor wired to a camera interface (device tree / ACPI) is built
        // in.
        candidate->internal = true;
        candidate->internal_source = "camera sensor";
    } else if (info.usb_removable.has_value()) {
        candidate->internal = !*info.usb_removable;
        candidate->internal_source =
                *info.usb_removable ? "removable USB port" : "built-in USB port";
    } else {
        candidate->internal = properties.default_internal;
        candidate->internal_source = "default";
    }

    candidate->facing_known = true;
    if (props.facing.has_value()) {
        candidate->facing = *props.facing;
        candidate->facing_source = "property";
    } else if (orientation == V4L2_CAMERA_ORIENTATION_FRONT ||
               orientation == V4L2_CAMERA_ORIENTATION_BACK) {
        candidate->facing =
                orientation == V4L2_CAMERA_ORIENTATION_FRONT ? Facing::kFront : Facing::kBack;
        candidate->facing_source = "firmware";
    } else if (candidate->hwdb.direction.has_value()) {
        candidate->facing = *candidate->hwdb.direction;
        candidate->facing_source = "hwdb";
    } else if (info.usb_removable == false) {
        // A built-in USB camera is almost always the one above a laptop's or
        // tablet's screen.
        candidate->facing = Facing::kFront;
        candidate->facing_source = "built-in USB camera";
    } else {
        candidate->facing = Facing::kBack;
        candidate->facing_source = "default";
        candidate->facing_known = false;
    }

    if (props.rotation.has_value()) {
        candidate->rotation = *props.rotation;
    } else if (sensor_rotation.has_value() && *sensor_rotation % 90 == 0) {
        // V4L2 gives the counter-clockwise correction, Android the clockwise
        // one.
        candidate->rotation = (360 - *sensor_rotation % 360) % 360;
    } else {
        candidate->rotation = 0;
    }
}

int64_t LargestArea(const CameraCandidate& candidate) {
    int64_t area = 0;
    for (const auto& format : candidate.formats) {
        for (const auto& size : format.sizes) area = std::max<int64_t>(area, size.Area());
    }
    return area;
}

// facing_by_resolution: of the internal cameras whose facing is not known,
// the one with the smallest resolution faces front.
void ApplyFacingByResolution(std::vector<CameraCandidate>* cameras) {
    std::vector<CameraCandidate*> unknown;
    for (auto& camera : *cameras) {
        if (camera.internal && !camera.facing_known) unknown.push_back(&camera);
    }
    if (unknown.size() < 2) return;
    auto smallest = *std::min_element(unknown.begin(), unknown.end(), [](auto* a, auto* b) {
        const int64_t area_a = LargestArea(*a);
        const int64_t area_b = LargestArea(*b);
        return area_a != area_b ? area_a < area_b : a->key < b->key;
    });
    for (auto* camera : unknown) {
        camera->facing = camera == smallest ? Facing::kFront : Facing::kBack;
        camera->facing_source = "resolution";
        LOG(INFO) << camera->info.name << ": facing "
                  << (camera->facing == Facing::kFront ? "front" : "back") << " by resolution";
    }
}

}  // namespace

std::vector<std::string> DeviceSelectors(const VideoDeviceInfo& info) {
    std::vector<std::string> selectors;
    selectors.push_back(Properties::SanitizeSelector(info.name));
    if (!info.bus_info.empty()) selectors.push_back(Properties::SanitizeSelector(info.bus_info));
    if (info.usb_vendor_id.has_value() && info.usb_product_id.has_value()) {
        selectors.push_back(::android::base::StringPrintf("usb:%04x:%04x", *info.usb_vendor_id,
                                                          *info.usb_product_id));
    }
    if (!info.card.empty()) selectors.push_back(Properties::SanitizeSelector(info.card));
    return selectors;
}

std::optional<CameraCandidate> ProbeCaptureNode(const Properties& properties,
                                                const CameraHwdb* hwdb, VideoDevice* device) {
    const VideoDeviceInfo& info = device->Info();
    const uint32_t caps = info.device_caps;
    const std::string what = info.name + " (" + info.driver + ", \"" + info.card + "\")";

    if (!(caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
        LOG(DEBUG) << what << ": not a video capture node";
        return std::nullopt;
    }
    if (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_VIDEO_OUTPUT |
                V4L2_CAP_VIDEO_OUTPUT_MPLANE)) {
        LOG(DEBUG) << what << ": memory to memory / output device (e.g. a codec)";
        return std::nullopt;
    }
    if (!(caps & V4L2_CAP_STREAMING)) {
        LOG(INFO) << what << ": no streaming I/O, not supported";
        return std::nullopt;
    }
    if (caps & V4L2_CAP_IO_MC) {
        // The pipeline in front of the node has to be configured through the
        // media controller (Qualcomm CAMSS, Intel IPU6, vimc, ...); such
        // cameras are found through their media device.
        LOG(DEBUG) << what << ": part of a media controller pipeline";
        return std::nullopt;
    }

    CameraCandidate candidate;
    candidate.key = CameraKey(info);
    candidate.info = info;
    candidate.selectors = DeviceSelectors(info);
    candidate.properties = Properties::LoadDeviceProperties(candidate.selectors);

    if (candidate.properties.enabled == false) {
        LOG(INFO) << what << ": disabled by property";
        return std::nullopt;
    }
    if (hwdb != nullptr && info.usb_vendor_id.has_value() && info.usb_product_id.has_value()) {
        // The V4L2 card name is the video device's name, which is what
        // systemd's rules look up as $attr{name}.
        candidate.hwdb = hwdb->Lookup(*info.usb_vendor_id, *info.usb_product_id, info.card);
    }
    // Infrared cameras (face unlock) are no use to camera apps.
    if (candidate.hwdb.infrared && !properties.include_ir && candidate.properties.enabled != true) {
        LOG(INFO) << what << ": infrared camera, skipped";
        return std::nullopt;
    }

    bool has_bayer = false;
    for (auto& format : device->EnumerateFormats()) {
        switch (ClassifyPixelFormat(format.fourcc)) {
            case PixelFormatClass::kProcessed:
                if (!format.sizes.empty()) candidate.formats.push_back(std::move(format));
                break;
            case PixelFormatClass::kBayer:
                has_bayer = true;
                break;
            case PixelFormatClass::kUnsupported:
                LOG(DEBUG) << what << ": ignoring unsupported format "
                           << FourccToString(format.fourcc);
                break;
        }
    }
    if (candidate.formats.empty()) {
        if (has_bayer) {
            LOG(INFO) << what << ": only raw Bayer formats, needs a software ISP (not supported "
                      << "yet), skipped";
        } else {
            LOG(INFO) << what << ": no supported pixel format, skipped";
        }
        return std::nullopt;
    }

    ResolvePlacement(properties, device, /*sensor=*/false, &candidate);
    candidate.prefer_rgb = candidate.properties.prefer_rgb.value_or(properties.prefer_rgb);
    candidate.advertise_rgb = candidate.properties.advertise_rgb.value_or(properties.advertise_rgb);

    LOG(INFO) << what << ": camera " << candidate.key << ", "
              << (candidate.internal ? "internal" : "external") << " (" << candidate.internal_source
              << ")";
    if (candidate.internal) {
        LOG(INFO) << what << ": facing " << (candidate.facing == Facing::kFront ? "front" : "back")
                  << " (" << candidate.facing_source << "), rotation " << candidate.rotation;
    }
    LOG(INFO) << what << ": selectors [" << ::android::base::Join(candidate.selectors, ", ") << "]";
    return candidate;
}

std::vector<CameraCandidate> ProbeMediaDevice(const Properties& properties, MediaDevice* media,
                                              const DeviceOpeners& open) {
    std::vector<CameraCandidate> candidates;
    const MediaTopology& topology = media->Topology();
    for (auto& camera : DiscoverMediaCameras(media, open)) {
        if (camera.pipeline == nullptr) continue;
        const MediaPipeline& pipeline = *camera.pipeline;
        const std::string what = media->Path() + ": sensor \"" + pipeline.sensor_entity + "\"";

        auto node = open.video(pipeline.video_node);
        if (!node.ok()) {
            LOG(WARNING) << what << ": " << node.error().message();
            continue;
        }
        auto sensor = open.subdev(pipeline.sensor_subdev);
        if (!sensor.ok()) {
            LOG(WARNING) << what << ": " << sensor.error().message();
            continue;
        }

        CameraCandidate candidate;
        candidate.key = (topology.bus_info.empty() ? topology.model : topology.bus_info) + "|" +
                        pipeline.sensor_entity;
        candidate.info = (*node)->Info();
        candidate.selectors = MediaSelectors(pipeline);
        candidate.properties = Properties::LoadDeviceProperties(candidate.selectors);
        if (candidate.properties.enabled == false) {
            LOG(INFO) << what << ": disabled by property";
            continue;
        }
        candidate.formats = std::move(camera.formats);
        candidate.pipeline = camera.pipeline;

        ResolvePlacement(properties, sensor->get(), /*sensor=*/true, &candidate);
        candidate.prefer_rgb = candidate.properties.prefer_rgb.value_or(properties.prefer_rgb);
        candidate.advertise_rgb =
                candidate.properties.advertise_rgb.value_or(properties.advertise_rgb);

        LOG(INFO) << what << ": camera " << candidate.key << ", "
                  << (candidate.internal ? "internal" : "external") << " ("
                  << candidate.internal_source << ")";
        if (candidate.internal) {
            LOG(INFO) << what << ": facing "
                      << (candidate.facing == Facing::kFront ? "front" : "back") << " ("
                      << candidate.facing_source << "), rotation " << candidate.rotation;
        }
        LOG(INFO) << what << ": selectors [" << ::android::base::Join(candidate.selectors, ", ")
                  << "]";
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

std::vector<std::string> MediaSelectors(const MediaPipeline& pipeline) {
    // "ov5675 2-0036": the sensor at an address, and the sensor model.
    std::vector<std::string> selectors = {Properties::SanitizeSelector(pipeline.sensor_entity)};
    const std::string model = pipeline.sensor_entity.substr(0, pipeline.sensor_entity.find(' '));
    if (!model.empty() && model != pipeline.sensor_entity) {
        selectors.push_back(Properties::SanitizeSelector(model));
    }
    return selectors;
}

void AssignFlashLeds(std::vector<CameraCandidate>* cameras,
                     const std::vector<std::string>& led_class) {
    bool configured = false;
    for (auto& camera : *cameras) {
        camera.flash_leds.clear();
        if (camera.properties.flash_led.has_value()) {
            camera.flash_leds = *camera.properties.flash_led;
            configured = true;
        } else if (camera.pipeline != nullptr && !camera.pipeline->flash_subdevs.empty()) {
            camera.flash_leds = camera.pipeline->flash_subdevs;
            configured = true;
        }
    }
    if (configured || led_class.empty()) return;

    CameraCandidate* back = nullptr;
    for (auto& camera : *cameras) {
        if (!camera.internal || camera.facing != Facing::kBack) continue;
        if (back == nullptr || camera.key < back->key) back = &camera;
    }
    if (back == nullptr) {
        LOG(DEBUG) << "no internal back camera for flash LED(s) "
                   << ::android::base::Join(led_class, ", ");
        return;
    }
    back->flash_leds = led_class;
}

DiscoveryResult DiscoverCameras(const Properties& properties, const CameraHwdb* hwdb,
                                const std::vector<CameraCandidate>& known,
                                const DeviceOpeners& open, const std::string& dev_dir,
                                const std::string& leds_dir) {
    DiscoveryResult result;
    std::set<std::string> keys;
    auto add = [&](CameraCandidate candidate) {
        if (keys.insert(candidate.key).second) result.cameras.push_back(std::move(candidate));
    };
    auto handle_open_error = [&](const ::android::base::ResultError<>& error) {
        const int code = error.code().value();
        if (IsTransientOpenError(code)) {
            LOG(DEBUG) << error.message() << ", will retry";
            result.retry = true;
        } else if (code != ENOENT) {
            LOG(WARNING) << error.message();
        }
    };

    // Sensors behind media controllers.
    for (const std::string& path : ListNodes(dev_dir, "media")) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;  // Removed meanwhile.

        bool reused = false;
        for (const auto& candidate : known) {
            if (candidate.pipeline != nullptr && candidate.pipeline->media_path == path &&
                candidate.pipeline->media_rdev == st.st_rdev) {
                add(candidate);
                reused = true;
            }
        }
        if (reused) continue;

        auto media = open.media(path);
        if (!media.ok()) {
            handle_open_error(media.error());
            continue;
        }
        for (auto& candidate : ProbeMediaDevice(properties, media->get(), open)) {
            add(std::move(candidate));
        }
    }

    // Plain capture nodes (USB cameras, capture cards).
    for (const std::string& path : ListNodes(dev_dir, "video")) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;  // Removed meanwhile.

        const auto it = std::find_if(known.begin(), known.end(), [&](const auto& candidate) {
            return candidate.pipeline == nullptr && candidate.info.path == path &&
                   candidate.info.rdev == st.st_rdev;
        });
        if (it != known.end()) {
            add(*it);
            continue;
        }

        auto device = open.video(path);
        if (!device.ok()) {
            handle_open_error(device.error());
            continue;
        }

        auto candidate = ProbeCaptureNode(properties, hwdb, device->get());
        if (!candidate.has_value()) continue;
        if (keys.count(candidate->key) != 0) {
            // e.g. the second capture node of a capture card.
            LOG(INFO) << path << ": another capture node of " << candidate->key
                      << " is used already, ignored";
            continue;
        }
        add(std::move(*candidate));
    }
    if (properties.facing_by_resolution) ApplyFacingByResolution(&result.cameras);
    AssignFlashLeds(&result.cameras, ListFlashLeds(leds_dir));
    for (const auto& camera : result.cameras) {
        if (!camera.flash_leds.empty()) {
            LOG(DEBUG) << camera.key << ": flash "
                       << ::android::base::Join(camera.flash_leds, ", ");
        }
    }
    return result;
}

}  // namespace aidl::android::hardware::camera::mainline
