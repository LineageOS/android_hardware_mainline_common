/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Media"

#include "v4l2/MediaDevice.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/media.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cstring>
#include <map>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include "v4l2/Controls.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::android::base::ErrnoError;
using ::android::base::Error;
using ::android::base::Result;
using ::android::base::unique_fd;

std::string CString(const char* data, size_t size) {
    return std::string(data, strnlen(data, size));
}

// Device nodes of V4L2 video nodes and sub-devices in `dev_dir`, by number.
std::map<dev_t, std::string> V4l2DeviceNodes(const std::string& dev_dir) {
    std::map<dev_t, std::string> nodes;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(dev_dir.c_str()), closedir);
    if (dir == nullptr) return nodes;
    while (const dirent* entry = readdir(dir.get())) {
        if (!::android::base::StartsWith(entry->d_name, "video") &&
            !::android::base::StartsWith(entry->d_name, "v4l-subdev")) {
            continue;
        }
        const std::string path = dev_dir + "/" + entry->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode)) nodes[st.st_rdev] = path;
    }
    return nodes;
}

class V4l2MediaDevice : public MediaDevice {
  public:
    V4l2MediaDevice(unique_fd fd, std::string path, dev_t rdev, MediaTopology topology)
        : fd_(std::move(fd)), path_(std::move(path)), rdev_(rdev), topology_(std::move(topology)) {}

    const std::string& Path() const override { return path_; }
    dev_t Rdev() const override { return rdev_; }
    const MediaTopology& Topology() const override { return topology_; }

    Result<void> SetupLink(const MediaTopology::Link& link, bool enable) override {
        const auto* source = topology_.FindPad(link.source);
        const auto* sink = topology_.FindPad(link.sink);
        if (source == nullptr || sink == nullptr) {
            return Error(EINVAL) << path_ << ": link " << link.id << " is no data link";
        }
        media_link_desc desc = {};
        desc.source.entity = source->entity;
        desc.source.index = static_cast<uint16_t>(source->index);
        desc.source.flags = MEDIA_PAD_FL_SOURCE;
        desc.sink.entity = sink->entity;
        desc.sink.index = static_cast<uint16_t>(sink->index);
        desc.sink.flags = MEDIA_PAD_FL_SINK;
        desc.flags = (link.flags & ~MEDIA_LNK_FL_ENABLED) | (enable ? MEDIA_LNK_FL_ENABLED : 0);
        if (Xioctl(fd_.get(), MEDIA_IOC_SETUP_LINK, &desc) != 0) {
            return ErrnoError() << path_ << ": " << (enable ? "enabling" : "disabling") << " link "
                                << link.id;
        }
        return {};
    }

  private:
    unique_fd fd_;
    std::string path_;
    dev_t rdev_;
    MediaTopology topology_;
};

Result<MediaTopology> ReadTopology(int fd, const std::string& dev_dir) {
    media_device_info info = {};
    if (Xioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) != 0)
        return ErrnoError() << "MEDIA_IOC_DEVICE_INFO";

    MediaTopology topology;
    topology.driver = CString(info.driver, sizeof(info.driver));
    topology.model = CString(info.model, sizeof(info.model));
    topology.bus_info = CString(info.bus_info, sizeof(info.bus_info));
    const bool has_pad_index = MEDIA_V2_PAD_HAS_INDEX(info.media_version);

    // The graph may change between the two calls (hotplug); retry then.
    for (int attempt = 0; attempt < 3; ++attempt) {
        media_v2_topology counts = {};
        if (Xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &counts) != 0) {
            return ErrnoError() << "MEDIA_IOC_G_TOPOLOGY";
        }
        std::vector<media_v2_entity> entities(counts.num_entities);
        std::vector<media_v2_interface> interfaces(counts.num_interfaces);
        std::vector<media_v2_pad> pads(counts.num_pads);
        std::vector<media_v2_link> links(counts.num_links);
        media_v2_topology full = counts;
        full.ptr_entities = reinterpret_cast<uintptr_t>(entities.data());
        full.ptr_interfaces = reinterpret_cast<uintptr_t>(interfaces.data());
        full.ptr_pads = reinterpret_cast<uintptr_t>(pads.data());
        full.ptr_links = reinterpret_cast<uintptr_t>(links.data());
        if (Xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &full) != 0) {
            if (errno == ENOSPC) continue;
            return ErrnoError() << "MEDIA_IOC_G_TOPOLOGY";
        }
        if (full.topology_version != counts.topology_version) continue;

        const auto nodes = V4l2DeviceNodes(dev_dir);
        std::map<uint32_t, std::string> interface_nodes;
        for (const auto& intf : interfaces) {
            if (intf.intf_type != MEDIA_INTF_T_V4L_VIDEO &&
                intf.intf_type != MEDIA_INTF_T_V4L_SUBDEV) {
                continue;
            }
            auto it = nodes.find(makedev(intf.devnode.major, intf.devnode.minor));
            if (it != nodes.end()) interface_nodes[intf.id] = it->second;
        }

        for (const auto& entity : entities) {
            topology.entities.push_back(
                    {entity.id, CString(entity.name, sizeof(entity.name)), entity.function, {}});
        }
        std::map<uint32_t, uint32_t> next_index;
        for (const auto& pad : pads) {
            const uint32_t index = has_pad_index ? pad.index : next_index[pad.entity_id]++;
            topology.pads.push_back({pad.id, pad.entity_id, index, pad.flags});
        }
        for (const auto& link : links) {
            if ((link.flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_INTERFACE_LINK) {
                // Interface to entity: where the entity's device node is.
                auto node = interface_nodes.find(link.source_id);
                if (node == interface_nodes.end()) continue;
                for (auto& entity : topology.entities) {
                    if (entity.id == link.sink_id) entity.devnode = node->second;
                }
                continue;
            }
            topology.links.push_back({link.id, link.source_id, link.sink_id, link.flags});
        }
        return topology;
    }
    return Error(EAGAIN) << "topology kept changing";
}

}  // namespace

const MediaTopology::Entity* MediaTopology::FindEntity(uint32_t id) const {
    for (const auto& entity : entities) {
        if (entity.id == id) return &entity;
    }
    return nullptr;
}

const MediaTopology::Pad* MediaTopology::FindPad(uint32_t id) const {
    for (const auto& pad : pads) {
        if (pad.id == id) return &pad;
    }
    return nullptr;
}

Result<std::unique_ptr<MediaDevice>> OpenMediaDevice(const std::string& path,
                                                     const std::string& dev_dir) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDWR | O_CLOEXEC)));
    if (fd.get() < 0) return ErrnoError() << "open " << path;
    struct stat st;
    if (fstat(fd.get(), &st) != 0) return ErrnoError() << "fstat " << path;

    auto topology = ReadTopology(fd.get(), dev_dir);
    if (!topology.ok())
        return Error(topology.error().code()) << path << ": " << topology.error().message();
    std::unique_ptr<MediaDevice> device = std::make_unique<V4l2MediaDevice>(
            std::move(fd), path, st.st_rdev, std::move(*topology));
    return device;
}

}  // namespace aidl::android::hardware::camera::mainline
