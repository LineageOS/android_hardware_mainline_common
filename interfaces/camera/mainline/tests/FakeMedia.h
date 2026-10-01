/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "v4l2/DeviceOpeners.h"

namespace aidl::android::hardware::camera::mainline {

// An in-memory media graph with its sub-devices and video nodes, handing out
// fake devices through DeviceOpeners. State persists across opens, like the
// kernel's.
class FakeMediaGraph {
  public:
    struct SubDeviceState {
        // Codes per pad, sizes per (pad, code).
        std::map<uint32_t, std::vector<uint32_t>> codes;
        std::map<std::pair<uint32_t, uint32_t>, std::vector<FrameSize>> sizes;
        // Active formats per pad, as set.
        std::map<uint32_t, MbusFormat> formats;
        std::map<uint32_t, int32_t> controls;
        int set_format_calls = 0;
    };

    explicit FakeMediaGraph(std::string path = "/dev/media0");

    // Builders. Pads are numbered per entity from 0; IDs are made up.
    uint32_t AddEntity(const std::string& name, uint32_t function, const std::string& devnode,
                       int sink_pads, int source_pads);
    // Links source pad `source_pad` of `source` to sink pad `sink_pad` of `sink`.
    uint32_t AddLink(uint32_t source, uint32_t source_pad, uint32_t sink, uint32_t sink_pad,
                     uint32_t flags);
    SubDeviceState& SubDevice(const std::string& devnode) { return subdevs_[devnode]; }
    // Formats a video node offers for a media bus code.
    void SetVideoFormats(const std::string& devnode, uint32_t code,
                         std::vector<FormatDescription> formats);

    DeviceOpeners Openers();
    const MediaTopology& topology() const { return topology_; }
    MediaTopology& topology() { return topology_; }
    // (link ID, enabled) of every SetupLink() call, in order.
    const std::vector<std::pair<uint32_t, bool>>& link_changes() const { return link_changes_; }

  private:
    friend class FakeMediaDevice;
    friend class FakeSubDevice;

    std::string path_;
    MediaTopology topology_;
    uint32_t next_id_ = 1;
    std::map<std::string, SubDeviceState> subdevs_;
    std::map<std::string, std::map<uint32_t, std::vector<FormatDescription>>> video_formats_;
    std::vector<std::pair<uint32_t, bool>> link_changes_;
};

}  // namespace aidl::android::hardware::camera::mainline
