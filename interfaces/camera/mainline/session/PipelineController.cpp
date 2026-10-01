/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Pipeline"

#include "session/PipelineController.h"

#include <linux/media.h>

#include <cerrno>

#include <android-base/logging.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::android::base::Error;
using ::android::base::Result;

bool IsEnabled(const MediaTopology::Link& link) {
    return (link.flags & MEDIA_LNK_FL_ENABLED) != 0;
}

bool IsImmutable(const MediaTopology::Link& link) {
    return (link.flags & MEDIA_LNK_FL_IMMUTABLE) != 0;
}

}  // namespace

Result<std::unique_ptr<PipelineController>> PipelineController::Open(
        std::shared_ptr<const MediaPipeline> pipeline, const DeviceOpeners& open) {
    auto media = open.media(pipeline->media_path);
    if (!media.ok()) return media.error();
    auto sensor = open.subdev(pipeline->sensor_subdev);
    if (!sensor.ok()) return sensor.error();
    return std::unique_ptr<PipelineController>(new PipelineController(
            std::move(pipeline), open, std::move(*media), std::move(*sensor)));
}

PipelineController::PipelineController(std::shared_ptr<const MediaPipeline> pipeline,
                                       DeviceOpeners open, std::unique_ptr<MediaDevice> media,
                                       std::unique_ptr<SubDevice> sensor)
    : pipeline_(std::move(pipeline)),
      open_(std::move(open)),
      media_(std::move(media)),
      sensor_(std::move(sensor)) {}

Result<SubDevice*> PipelineController::Stage(const std::string& path) {
    auto it = stages_.find(path);
    if (it == stages_.end()) {
        auto stage = open_.subdev(path);
        if (!stage.ok()) return stage.error();
        it = stages_.emplace(path, std::move(*stage)).first;
    }
    return it->second.get();
}

Result<void> PipelineController::EnableLinks() {
    // The current state of the links, which other cameras of the same media
    // device may have changed.
    const MediaTopology& topology = media_->Topology();
    for (const auto& hop : pipeline_->hops) {
        const MediaTopology::Link* link = nullptr;
        for (const auto& candidate : topology.links) {
            if (candidate.id == hop.link.id) link = &candidate;
        }
        if (link == nullptr) return Error(ENODEV) << "link " << hop.link.id << " is gone";

        // A sink pad takes one stream: disable whatever else feeds it.
        for (const auto& other : topology.links) {
            if (other.id == link->id || other.sink != link->sink || !IsEnabled(other) ||
                IsImmutable(other)) {
                continue;
            }
            if (auto result = media_->SetupLink(other, false); !result.ok()) return result;
        }
        if (!IsEnabled(*link) && !IsImmutable(*link)) {
            if (auto result = media_->SetupLink(*link, true); !result.ok()) return result;
        }
    }
    return {};
}

Result<Size> PipelineController::Configure(uint32_t fourcc, Size size) {
    const MediaPipeline::Format* format = pipeline_->FindFormat(fourcc);
    if (format == nullptr) return Error(EINVAL) << "the pipeline does not deliver this format";

    // Reopen the media device so that its topology is current.
    auto media = open_.media(pipeline_->media_path);
    if (!media.ok()) return media.error();
    media_ = std::move(*media);
    if (auto result = EnableLinks(); !result.ok()) return result.error();

    auto current = sensor_->SetFormat(pipeline_->sensor_pad,
                                      {format->sensor_code, static_cast<uint32_t>(size.width),
                                       static_cast<uint32_t>(size.height)});
    if (!current.ok()) return current.error();
    MbusFormat stream = *current;
    LOG(DEBUG) << pipeline_->sensor_entity << ": 0x" << std::hex << stream.code << std::dec << " "
               << stream.width << "x" << stream.height;

    // Every stage takes what the previous one sends and passes it on,
    // converted where it is a converting stage.
    for (const auto& hop : pipeline_->hops) {
        if (hop.sink_subdev.empty()) break;  // The video node.
        auto stage = Stage(hop.sink_subdev);
        if (!stage.ok()) return stage.error();
        auto sink = (*stage)->SetFormat(hop.sink_pad, stream);
        if (!sink.ok()) return sink.error();

        const uint32_t code =
                hop.converter && format->output_code != 0 ? format->output_code : sink->code;
        auto source = (*stage)->SetFormat(hop.source_pad, {code, sink->width, sink->height});
        if (!source.ok()) {
            // Some stages derive their source format from the sink.
            LOG(DEBUG) << source.error().message();
            source = (*stage)->GetFormat(hop.source_pad);
            if (!source.ok()) return source.error();
        }
        stream = *source;
        LOG(DEBUG) << hop.sink_entity << ": 0x" << std::hex << stream.code << std::dec << " "
                   << stream.width << "x" << stream.height;
    }
    return Size{static_cast<int32_t>(stream.width), static_cast<int32_t>(stream.height)};
}

Result<Fraction> PipelineController::SetFrameInterval(const Fraction& interval) {
    return sensor_->SetFrameInterval(pipeline_->sensor_pad, interval);
}

}  // namespace aidl::android::hardware::camera::mainline
