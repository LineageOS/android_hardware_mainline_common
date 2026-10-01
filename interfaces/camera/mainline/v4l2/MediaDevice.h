/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <android-base/result.h>

namespace aidl::android::hardware::camera::mainline {

// The graph of a media controller device, as reported by
// MEDIA_IOC_G_TOPOLOGY.
struct MediaTopology {
    struct Entity {
        uint32_t id = 0;
        std::string name;
        // MEDIA_ENT_F_*
        uint32_t function = 0;
        // Device node of its V4L2 interface (video node or sub-device), empty
        // if it has none or it could not be found.
        std::string devnode;
    };
    struct Pad {
        uint32_t id = 0;
        uint32_t entity = 0;
        // Index of the pad within its entity, as used by the sub-device API.
        uint32_t index = 0;
        // MEDIA_PAD_FL_*
        uint32_t flags = 0;
    };
    // Data links (pad to pad) and ancillary links (entity to entity, e.g. a
    // sensor to its flash and lens).
    struct Link {
        uint32_t id = 0;
        // Pad IDs for data links, entity IDs for ancillary links.
        uint32_t source = 0;
        uint32_t sink = 0;
        // MEDIA_LNK_FL_*
        uint32_t flags = 0;
    };

    std::string driver;
    std::string model;
    std::string bus_info;
    std::vector<Entity> entities;
    std::vector<Pad> pads;
    std::vector<Link> links;

    const Entity* FindEntity(uint32_t id) const;
    const Pad* FindPad(uint32_t id) const;
};

// A media controller device (/dev/mediaN). Abstract for the unit tests.
class MediaDevice {
  public:
    virtual ~MediaDevice() = default;

    virtual const std::string& Path() const = 0;
    // Device number of the node, to detect a node that was replaced.
    virtual dev_t Rdev() const = 0;
    virtual const MediaTopology& Topology() const = 0;

    // Enables or disables a data link (MEDIA_IOC_SETUP_LINK).
    virtual ::android::base::Result<void> SetupLink(const MediaTopology::Link& link,
                                                    bool enable) = 0;
};

using MediaDeviceOpener =
        std::function<::android::base::Result<std::unique_ptr<MediaDevice>>(const std::string&)>;

// Opens a media device and reads its topology. Device nodes of interfaces are
// looked up in `dev_dir` by device number.
::android::base::Result<std::unique_ptr<MediaDevice>> OpenMediaDevice(
        const std::string& path, const std::string& dev_dir = "/dev");

}  // namespace aidl::android::hardware::camera::mainline
