/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tests/FakeMedia.h"

#include <linux/media.h>
#include <linux/videodev2.h>

#include "tests/FakeVideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

using ::android::base::Error;
using ::android::base::Result;

class FakeMediaDevice : public MediaDevice {
  public:
    explicit FakeMediaDevice(FakeMediaGraph* graph) : graph_(graph) {}

    const std::string& Path() const override { return graph_->path_; }
    dev_t Rdev() const override { return 42; }
    const MediaTopology& Topology() const override { return graph_->topology_; }

    Result<void> SetupLink(const MediaTopology::Link& link, bool enable) override {
        for (auto& candidate : graph_->topology_.links) {
            if (candidate.id != link.id) continue;
            if (candidate.flags & MEDIA_LNK_FL_IMMUTABLE) return Error(EINVAL) << "immutable";
            candidate.flags = enable ? (candidate.flags | MEDIA_LNK_FL_ENABLED)
                                     : (candidate.flags & ~MEDIA_LNK_FL_ENABLED);
            graph_->link_changes_.emplace_back(link.id, enable);
            return {};
        }
        return Error(EINVAL) << "no link " << link.id;
    }

  private:
    FakeMediaGraph* graph_;
};

class FakeSubDevice : public SubDevice {
  public:
    FakeSubDevice(FakeMediaGraph* graph, std::string path)
        : graph_(graph), path_(std::move(path)) {}

    const std::string& Path() const override { return path_; }

    bool HasControl(uint32_t id) override { return State().controls.count(id) != 0; }
    std::optional<int32_t> GetControl(uint32_t id) override {
        auto it = State().controls.find(id);
        if (it == State().controls.end()) return std::nullopt;
        return it->second;
    }
    bool SetControl(uint32_t id, int32_t value) override {
        auto it = State().controls.find(id);
        if (it == State().controls.end()) return false;
        it->second = value;
        return true;
    }

    std::vector<uint32_t> EnumerateCodes(uint32_t pad) override { return State().codes[pad]; }
    std::vector<FrameSize> EnumerateSizes(uint32_t pad, uint32_t code) override {
        return State().sizes[{pad, code}];
    }
    Result<MbusFormat> GetFormat(uint32_t pad) override {
        auto it = State().formats.find(pad);
        if (it == State().formats.end()) return Error(EINVAL) << "no format on pad " << pad;
        return it->second;
    }
    Result<MbusFormat> SetFormat(uint32_t pad, const MbusFormat& format) override {
        ++State().set_format_calls;
        State().formats[pad] = format;
        return format;
    }
    Result<Fraction> SetFrameInterval(uint32_t /*pad*/, const Fraction& interval) override {
        return interval;
    }

  private:
    FakeMediaGraph::SubDeviceState& State() { return graph_->subdevs_[path_]; }

    FakeMediaGraph* graph_;
    std::string path_;
};

FakeMediaGraph::FakeMediaGraph(std::string path) : path_(std::move(path)) {
    topology_.model = "fake";
    topology_.bus_info = "platform:fake";
}

uint32_t FakeMediaGraph::AddEntity(const std::string& name, uint32_t function,
                                   const std::string& devnode, int sink_pads, int source_pads) {
    const uint32_t id = next_id_++;
    topology_.entities.push_back({id, name, function, devnode});
    uint32_t index = 0;
    for (int i = 0; i < sink_pads; ++i) {
        topology_.pads.push_back({next_id_++, id, index++, MEDIA_PAD_FL_SINK});
    }
    for (int i = 0; i < source_pads; ++i) {
        topology_.pads.push_back({next_id_++, id, index++, MEDIA_PAD_FL_SOURCE});
    }
    return id;
}

uint32_t FakeMediaGraph::AddLink(uint32_t source, uint32_t source_pad, uint32_t sink,
                                 uint32_t sink_pad, uint32_t flags) {
    auto pad_id = [&](uint32_t entity, uint32_t index) {
        for (const auto& pad : topology_.pads) {
            if (pad.entity == entity && pad.index == index) return pad.id;
        }
        return 0u;
    };
    const uint32_t id = next_id_++;
    topology_.links.push_back({id, pad_id(source, source_pad), pad_id(sink, sink_pad),
                               flags | MEDIA_LNK_FL_DATA_LINK});
    return id;
}

void FakeMediaGraph::SetVideoFormats(const std::string& devnode, uint32_t code,
                                     std::vector<FormatDescription> formats) {
    video_formats_[devnode][code] = std::move(formats);
}

DeviceOpeners FakeMediaGraph::Openers() {
    DeviceOpeners open;
    open.media = [this](const std::string& path) -> Result<std::unique_ptr<MediaDevice>> {
        if (path != path_) return Error(ENOENT) << "no media device " << path;
        return std::make_unique<FakeMediaDevice>(this);
    };
    open.subdev = [this](const std::string& path) -> Result<std::unique_ptr<mainline::SubDevice>> {
        return std::make_unique<FakeSubDevice>(this, path);
    };
    open.video = [this](const std::string& path) -> Result<std::unique_ptr<VideoDevice>> {
        VideoDeviceInfo info;
        info.path = path;
        info.name = path.substr(path.rfind('/') + 1);
        info.driver = "fake";
        info.card = "fake";
        info.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_IO_MC;
        auto device = std::make_unique<FakeVideoDevice>(info, std::vector<FormatDescription>{});
        for (const auto& [code, formats] : video_formats_[path]) {
            device->SetFormatsForCode(code, formats);
        }
        return device;
    };
    return open;
}

}  // namespace aidl::android::hardware::camera::mainline
