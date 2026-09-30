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

constexpr char kVideoPrefix[] = "video";

// Number of a "videoN" node name, nullopt for anything else.
std::optional<unsigned> VideoNodeNumber(const std::string& name) {
    if (!::android::base::StartsWith(name, kVideoPrefix)) return std::nullopt;
    unsigned number;
    if (!::android::base::ParseUint(name.substr(sizeof(kVideoPrefix) - 1), &number)) {
        return std::nullopt;
    }
    return number;
}

// "videoN" nodes in `dev_dir`, in numerical order.
std::vector<std::string> ListVideoNodes(const std::string& dev_dir) {
    std::vector<std::pair<unsigned, std::string>> nodes;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(dev_dir.c_str()), closedir);
    if (dir == nullptr) {
        PLOG(ERROR) << "failed to open " << dev_dir;
        return {};
    }
    while (const dirent* entry = readdir(dir.get())) {
        const std::string name = entry->d_name;
        if (auto number = VideoNodeNumber(name); number.has_value()) {
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

std::optional<CameraCandidate> ProbeCaptureNode(const Properties& properties, VideoDevice* device) {
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
        // media controller (Qualcomm CAMSS, Intel IPU6, ...).
        LOG(INFO) << what << ": part of a media controller pipeline, not supported yet";
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

    candidate.internal = candidate.properties.internal.value_or(properties.default_internal);
    candidate.facing = candidate.properties.facing.value_or(Facing::kBack);
    candidate.rotation = candidate.properties.rotation.value_or(0);

    LOG(INFO) << what << ": camera " << candidate.key << ", "
              << (candidate.internal ? (candidate.facing == Facing::kFront ? "internal front"
                                                                           : "internal back")
                                     : "external")
              << ", rotation " << candidate.rotation << ", selectors ["
              << ::android::base::Join(candidate.selectors, ", ") << "]";
    return candidate;
}

DiscoveryResult DiscoverCameras(const Properties& properties,
                                const std::map<std::string, CameraCandidate>& known,
                                const VideoDeviceOpener& open, const std::string& dev_dir) {
    DiscoveryResult result;
    std::set<std::string> keys;

    for (const std::string& path : ListVideoNodes(dev_dir)) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;  // Removed meanwhile.

        if (auto it = known.find(path); it != known.end() && it->second.info.rdev == st.st_rdev) {
            if (keys.insert(it->second.key).second) result.cameras.push_back(it->second);
            continue;
        }

        auto device = open(path);
        if (!device.ok()) {
            const int error = device.error().code().value();
            if (IsTransientOpenError(error)) {
                LOG(DEBUG) << device.error().message() << ", will retry";
                result.retry = true;
            } else if (error != ENOENT) {
                LOG(WARNING) << device.error().message();
            }
            continue;
        }

        auto candidate = ProbeCaptureNode(properties, device->get());
        if (!candidate.has_value()) continue;
        if (!keys.insert(candidate->key).second) {
            // e.g. the second capture node of a capture card.
            LOG(INFO) << path << ": another capture node of " << candidate->key
                      << " is used already, ignored";
            continue;
        }
        result.cameras.push_back(std::move(*candidate));
    }
    return result;
}

}  // namespace aidl::android::hardware::camera::mainline
