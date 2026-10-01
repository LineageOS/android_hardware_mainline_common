/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_MediaPipeline"

#include "provider/MediaPipeline.h"

#include <linux/media-bus-format.h>
#include <linux/media.h>
#include <linux/videodev2.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include <android-base/logging.h>
#include <android-base/strings.h>

#include "isp/BayerFormat.h"
#include "v4l2/PixelFormats.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using Link = MediaTopology::Link;

// Stages between a sensor and a video node at most.
constexpr int kMaxDepth = 8;

// Frame interval assumed for sensor modes without any.
constexpr Fraction kDefaultInterval = {1, 30};

bool IsDataLink(const Link& link) {
    return (link.flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_DATA_LINK;
}

// A link that is or can be enabled.
bool IsUsable(const Link& link) {
    return (link.flags & MEDIA_LNK_FL_ENABLED) || !(link.flags & MEDIA_LNK_FL_IMMUTABLE);
}

// Device nodes of the flash sub-devices with an ancillary link from a sensor
// (the "flash-leds" of the sensor's firmware node).
std::vector<std::string> LinkedFlashes(const MediaTopology& topology, uint32_t sensor) {
    std::vector<std::string> flashes;
    for (const auto& link : topology.links) {
        if ((link.flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_ANCILLARY_LINK) continue;
        if (link.source != sensor) continue;
        const auto* flash = topology.FindEntity(link.sink);
        if (flash != nullptr && flash->function == MEDIA_ENT_F_FLASH && !flash->devnode.empty()) {
            flashes.push_back(flash->devnode);
        }
    }
    return flashes;
}

bool IsVideoNode(const MediaTopology::Entity& entity) {
    const std::string base = entity.devnode.substr(entity.devnode.rfind('/') + 1);
    return entity.function == MEDIA_ENT_F_IO_V4L || ::android::base::StartsWith(base, "video");
}

bool IsConverter(const MediaTopology::Entity& entity) {
    return entity.function == MEDIA_ENT_F_PROC_VIDEO_PIXEL_ENC_CONV ||
           entity.function == MEDIA_ENT_F_PROC_VIDEO_ISP;
}

// Depth first search for every path of usable data links from `entity` to a
// video node, not passing through other sensors.
void FindPaths(const MediaTopology& topology, uint32_t entity, std::set<uint32_t>* visited,
               std::vector<const Link*>* current, std::vector<std::vector<const Link*>>* paths) {
    if (current->size() >= kMaxDepth) return;
    for (const auto& pad : topology.pads) {
        if (pad.entity != entity || !(pad.flags & MEDIA_PAD_FL_SOURCE)) continue;
        for (const auto& link : topology.links) {
            if (!IsDataLink(link) || link.source != pad.id || !IsUsable(link)) continue;
            const auto* sink_pad = topology.FindPad(link.sink);
            if (sink_pad == nullptr) continue;
            const auto* sink = topology.FindEntity(sink_pad->entity);
            if (sink == nullptr || visited->count(sink->id) != 0) continue;
            if (sink->function == MEDIA_ENT_F_CAM_SENSOR) continue;
            // Every stage has to be configurable through its device node.
            if (sink->devnode.empty()) continue;

            current->push_back(&link);
            if (IsVideoNode(*sink)) {
                paths->push_back(*current);
            } else {
                visited->insert(sink->id);
                FindPaths(topology, sink->id, visited, current, paths);
                visited->erase(sink->id);
            }
            current->pop_back();
        }
    }
}

// Turns a path of links into the pipeline's hops.
MediaPipeline BuildPipeline(const MediaTopology& topology, const std::vector<const Link*>& path) {
    MediaPipeline pipeline;
    for (size_t i = 0; i < path.size(); ++i) {
        const auto* sink_pad = topology.FindPad(path[i]->sink);
        const auto* sink = topology.FindEntity(sink_pad->entity);
        MediaPipeline::Hop hop;
        hop.link = *path[i];
        hop.sink_entity = sink->name;
        hop.sink_pad = sink_pad->index;
        if (i + 1 < path.size()) {
            hop.sink_subdev = sink->devnode;
            hop.source_pad = topology.FindPad(path[i + 1]->source)->index;
            hop.converter = IsConverter(*sink);
        } else {
            pipeline.video_node = sink->devnode;
        }
        pipeline.hops.push_back(std::move(hop));
    }
    pipeline.sensor_pad = topology.FindPad(path.front()->source)->index;
    return pipeline;
}

// What video nodes deliver for media bus codes, looked up once per scan: the
// many paths of a media device (72 for a CAMSS sensor) share a few video
// nodes.
class VideoFormatCache {
  public:
    explicit VideoFormatCache(const DeviceOpeners& open) : open_(open) {}

    // Null when the node can not be opened (logged once).
    const std::vector<FormatDescription>* Formats(const std::string& node, uint32_t code) {
        auto [it, added] = cache_.try_emplace({node, code});
        if (added) {
            auto device = open_.video(node);
            if (device.ok()) {
                it->second = (*device)->EnumerateFormats(code);
            } else {
                LOG(WARNING) << device.error().message();
            }
        }
        return it->second.has_value() ? &*it->second : nullptr;
    }

  private:
    const DeviceOpeners& open_;
    std::map<std::pair<std::string, uint32_t>, std::optional<std::vector<FormatDescription>>>
            cache_;
};

// Works out the formats `pipeline` can deliver. Fills pipeline->formats and
// returns the matching format descriptions with the sensor's frame sizes.
//
// `raw`: look for raw Bayer formats the software ISP reads instead, on paths
// without a converting stage.
std::vector<FormatDescription> EvaluateFormats(MediaPipeline* pipeline, SubDevice* sensor,
                                               const DeviceOpeners& open, VideoFormatCache* cache,
                                               bool raw) {
    // (sensor code, output code at the converters, code at the video node)
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> candidates;
    const std::vector<uint32_t> sensor_codes = sensor->EnumerateCodes(pipeline->sensor_pad);
    const auto converter = std::find_if(pipeline->hops.begin(), pipeline->hops.end(),
                                        [](const auto& hop) { return hop.converter; });
    if (raw) {
        if (converter != pipeline->hops.end()) return {};
        for (const uint32_t code : sensor_codes) {
            if (IsBayerMbusCode(code)) candidates.emplace_back(code, 0, code);
        }
    } else if (converter != pipeline->hops.end()) {
        auto stage = open.subdev(converter->sink_subdev);
        if (!stage.ok()) {
            LOG(WARNING) << stage.error().message();
            return {};
        }
        for (const uint32_t output : (*stage)->EnumerateCodes(converter->source_pad)) {
            if (IsBayerMbusCode(output)) continue;
            for (const uint32_t code : sensor_codes) candidates.emplace_back(code, output, output);
        }
    } else {
        for (const uint32_t code : sensor_codes) {
            if (!IsBayerMbusCode(code)) candidates.emplace_back(code, 0, code);
        }
    }

    std::vector<FormatDescription> formats;
    for (const auto& [sensor_code, output_code, final_code] : candidates) {
        // What the video node makes of the code, cross-checked with what the
        // code can mean when the driver does not filter by media bus code.
        const std::vector<uint32_t> known = PixelFormatsForMbusCode(final_code);
        const auto* node_formats = cache->Formats(pipeline->video_node, final_code);
        if (node_formats == nullptr) return {};
        for (const auto& format : *node_formats) {
            if (raw ? !IsIspPixelFormat(format.fourcc) : !IsProcessedPixelFormat(format.fourcc)) {
                continue;
            }
            if (!known.empty() &&
                std::find(known.begin(), known.end(), format.fourcc) == known.end()) {
                continue;
            }
            if (pipeline->FindFormat(format.fourcc) != nullptr) continue;

            FormatDescription description;
            description.fourcc = format.fourcc;
            description.description = format.description;
            description.sizes = sensor->EnumerateSizes(pipeline->sensor_pad, sensor_code);
            for (auto& size : description.sizes) {
                if (size.intervals.empty()) size.intervals.push_back(kDefaultInterval);
            }
            if (description.sizes.empty()) continue;
            pipeline->formats.push_back({format.fourcc, sensor_code, output_code});
            formats.push_back(std::move(description));
        }
    }
    return formats;
}

size_t EnabledLinks(const MediaPipeline& pipeline) {
    return std::count_if(pipeline.hops.begin(), pipeline.hops.end(), [](const auto& hop) {
        return (hop.link.flags & MEDIA_LNK_FL_ENABLED) != 0;
    });
}

}  // namespace

const MediaPipeline::Format* MediaPipeline::FindFormat(uint32_t fourcc) const {
    for (const auto& format : formats) {
        if (format.fourcc == fourcc) return &format;
    }
    return nullptr;
}

std::vector<uint32_t> PixelFormatsForMbusCode(uint32_t code) {
    switch (code) {
        case MEDIA_BUS_FMT_YUYV8_2X8:
        case MEDIA_BUS_FMT_YUYV8_1X16:
            return {V4L2_PIX_FMT_YUYV};
        case MEDIA_BUS_FMT_YVYU8_2X8:
        case MEDIA_BUS_FMT_YVYU8_1X16:
            return {V4L2_PIX_FMT_YVYU};
        case MEDIA_BUS_FMT_UYVY8_2X8:
        case MEDIA_BUS_FMT_UYVY8_1X16:
            return {V4L2_PIX_FMT_UYVY};
        case MEDIA_BUS_FMT_VYUY8_2X8:
        case MEDIA_BUS_FMT_VYUY8_1X16:
            return {V4L2_PIX_FMT_VYUY};
        case MEDIA_BUS_FMT_RGB888_1X24:
        case MEDIA_BUS_FMT_BGR888_1X24:
            return {V4L2_PIX_FMT_RGB24,  V4L2_PIX_FMT_BGR24,  V4L2_PIX_FMT_XBGR32,
                    V4L2_PIX_FMT_XRGB32, V4L2_PIX_FMT_ABGR32, V4L2_PIX_FMT_ARGB32,
                    V4L2_PIX_FMT_RGBX32, V4L2_PIX_FMT_RGBA32};
        case MEDIA_BUS_FMT_RGB565_1X16:
            return {V4L2_PIX_FMT_RGB565};
        case MEDIA_BUS_FMT_Y8_1X8:
            return {V4L2_PIX_FMT_GREY};
        // Raw Bayer, for the software ISP.
        case MEDIA_BUS_FMT_SBGGR8_1X8:
            return {V4L2_PIX_FMT_SBGGR8};
        case MEDIA_BUS_FMT_SBGGR10_1X10:
            return {V4L2_PIX_FMT_SBGGR10P, V4L2_PIX_FMT_SBGGR10};
        case MEDIA_BUS_FMT_SBGGR12_1X12:
            return {V4L2_PIX_FMT_SBGGR12P, V4L2_PIX_FMT_SBGGR12};
        case MEDIA_BUS_FMT_SGBRG8_1X8:
            return {V4L2_PIX_FMT_SGBRG8};
        case MEDIA_BUS_FMT_SGBRG10_1X10:
            return {V4L2_PIX_FMT_SGBRG10P, V4L2_PIX_FMT_SGBRG10};
        case MEDIA_BUS_FMT_SGBRG12_1X12:
            return {V4L2_PIX_FMT_SGBRG12P, V4L2_PIX_FMT_SGBRG12};
        case MEDIA_BUS_FMT_SGRBG8_1X8:
            return {V4L2_PIX_FMT_SGRBG8};
        case MEDIA_BUS_FMT_SGRBG10_1X10:
            return {V4L2_PIX_FMT_SGRBG10P, V4L2_PIX_FMT_SGRBG10};
        case MEDIA_BUS_FMT_SGRBG12_1X12:
            return {V4L2_PIX_FMT_SGRBG12P, V4L2_PIX_FMT_SGRBG12};
        case MEDIA_BUS_FMT_SRGGB8_1X8:
            return {V4L2_PIX_FMT_SRGGB8};
        case MEDIA_BUS_FMT_SRGGB10_1X10:
            return {V4L2_PIX_FMT_SRGGB10P, V4L2_PIX_FMT_SRGGB10};
        case MEDIA_BUS_FMT_SRGGB12_1X12:
            return {V4L2_PIX_FMT_SRGGB12P, V4L2_PIX_FMT_SRGGB12};
        default:
            return {};
    }
}

std::vector<MediaCamera> DiscoverMediaCameras(MediaDevice* media, const DeviceOpeners& open,
                                              bool software_isp) {
    const MediaTopology& topology = media->Topology();
    std::vector<MediaCamera> cameras;
    VideoFormatCache cache(open);

    for (const auto& sensor : topology.entities) {
        if (sensor.function != MEDIA_ENT_F_CAM_SENSOR) continue;
        const std::string what = media->Path() + ": sensor \"" + sensor.name + "\"";
        MediaCamera camera;
        if (sensor.devnode.empty()) {
            LOG(INFO) << what << ": no sub-device node, skipped";
            cameras.push_back(std::move(camera));
            continue;
        }
        auto sensor_device = open.subdev(sensor.devnode);
        if (!sensor_device.ok()) {
            LOG(WARNING) << what << ": " << sensor_device.error().message();
            cameras.push_back(std::move(camera));
            continue;
        }

        std::set<uint32_t> visited = {sensor.id};
        std::vector<const Link*> current;
        std::vector<std::vector<const Link*>> paths;
        FindPaths(topology, sensor.id, &visited, &current, &paths);

        bool all_bayer = true;
        for (const auto& path : paths) {
            const uint32_t pad = topology.FindPad(path.front()->source)->index;
            for (const uint32_t code : (*sensor_device)->EnumerateCodes(pad)) {
                all_bayer &= IsBayerMbusCode(code);
            }
        }

        // The best path delivering processed (or, with `raw`, raw) formats.
        auto pick = [&](bool raw) {
            std::shared_ptr<MediaPipeline> best;
            size_t usable = 0;
            for (const auto& path : paths) {
                auto pipeline = std::make_shared<MediaPipeline>(BuildPipeline(topology, path));
                pipeline->media_path = media->Path();
                pipeline->media_rdev = media->Rdev();
                pipeline->media_model = topology.model;
                pipeline->sensor_entity = sensor.name;
                pipeline->sensor_subdev = sensor.devnode;
                auto formats =
                        EvaluateFormats(pipeline.get(), sensor_device->get(), open, &cache, raw);
                LOG(VERBOSE) << what << ": path to " << pipeline->video_node << " with "
                             << pipeline->hops.size() << " link(s): " << formats.size()
                             << " usable " << (raw ? "raw " : "") << "format(s)";
                if (formats.empty()) continue;
                ++usable;

                // Most formats, then the shortest path, then the one closest
                // to being set up already.
                const auto score = [](const MediaPipeline& p, size_t count) {
                    return std::make_tuple(count, -static_cast<int64_t>(p.hops.size()),
                                           EnabledLinks(p));
                };
                if (best == nullptr ||
                    score(*pipeline, formats.size()) > score(*best, camera.formats.size())) {
                    best = pipeline;
                    camera.formats = std::move(formats);
                }
            }
            LOG(DEBUG) << what << ": " << usable << " of " << paths.size() << " path(s) deliver "
                       << (raw ? "raw" : "processed") << " formats";
            return best;
        };
        std::shared_ptr<MediaPipeline> best = pick(/*raw=*/false);
        camera.raw_only = best == nullptr && all_bayer && !paths.empty();
        if (camera.raw_only && software_isp) {
            best = pick(/*raw=*/true);
            if (best != nullptr) LOG(INFO) << what << ": raw Bayer only, using the software ISP";
        }

        if (best == nullptr) {
            LOG(INFO) << what << ": "
                      << (paths.empty()      ? "no path to a video node"
                          : !camera.raw_only ? "no usable format"
                          : software_isp     ? "raw Bayer only, in no format the software ISP reads"
                                             : "raw Bayer only, software ISP disabled")
                      << ", skipped";
        } else {
            LOG(INFO) << what << ": captured from " << best->video_node << " through "
                      << best->hops.size() - 1 << " stage(s)";
            best->flash_subdevs = LinkedFlashes(topology, sensor.id);
            for (const auto& flash : best->flash_subdevs) {
                LOG(INFO) << what << ": flash " << flash;
            }
            camera.pipeline = std::move(best);
        }
        cameras.push_back(std::move(camera));
    }
    return cameras;
}

}  // namespace aidl::android::hardware::camera::mainline
