/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "v4l2/DeviceOpeners.h"

namespace aidl::android::hardware::camera::mainline {

// The path from a camera sensor through the stages of a media controller
// pipeline (Qualcomm CAMSS, vimc, ...) to the video node frames are captured
// from.
struct MediaPipeline {
    // One data link of the path.
    struct Hop {
        MediaTopology::Link link;
        // Entity the link leads to, and its pad.
        std::string sink_entity;
        uint32_t sink_pad = 0;
        // Device node of the sink entity: a sub-device for an intermediate
        // stage, empty for the final video node.
        std::string sink_subdev;
        // The pad the sink entity passes the stream on from (next hop's
        // source pad); unused for the video node.
        uint32_t source_pad = 0;
        // The stage converts the format (e.g. debayers): its source pad gets
        // the output code instead of the sink's code.
        bool converter = false;
    };

    // A capture pixel format the pipeline can deliver, and how.
    struct Format {
        uint32_t fourcc = 0;
        // Media bus code the sensor sends.
        uint32_t sensor_code = 0;
        // Code to set on the source pads of converting stages, 0 if none.
        uint32_t output_code = 0;
    };

    std::string media_path;
    dev_t media_rdev = 0;
    std::string media_model;
    std::string sensor_entity;
    std::string sensor_subdev;
    uint32_t sensor_pad = 0;
    std::vector<Hop> hops;
    std::string video_node;
    std::vector<Format> formats;
    // V4L2 flash sub-devices linked to the sensor (ancillary links).
    std::vector<std::string> flash_subdevs;

    const Format* FindFormat(uint32_t fourcc) const;
};

// A camera found behind a media controller: its pipeline, and the formats
// (with sensor frame sizes and intervals) the pipeline delivers.
struct MediaCamera {
    std::shared_ptr<const MediaPipeline> pipeline;
    std::vector<FormatDescription> formats;
    // Sensor that only offers raw Bayer data on every path; needs an ISP.
    bool raw_only = false;
};

// Finds every camera sensor of a media device and the best pipeline to a
// video node for each. Sensors without a usable pipeline are logged and come
// back with an empty `pipeline`.
std::vector<MediaCamera> DiscoverMediaCameras(MediaDevice* media, const DeviceOpeners& open);

// The pixel formats a media bus code can be captured as, ordered by
// preference; empty when unknown.
std::vector<uint32_t> PixelFormatsForMbusCode(uint32_t code);

}  // namespace aidl::android::hardware::camera::mainline
