/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <string>

#include <android-base/result.h>

#include "device/StreamPlanner.h"
#include "provider/MediaPipeline.h"

namespace aidl::android::hardware::camera::mainline {

// Sets up a camera's media controller pipeline for capturing: enables its
// links and configures the formats of its stages, and gives access to the
// sensor's controls.
class PipelineController {
  public:
    static ::android::base::Result<std::unique_ptr<PipelineController>> Open(
            std::shared_ptr<const MediaPipeline> pipeline, const DeviceOpeners& open);

    // Configures the pipeline to deliver `fourcc` from sensor frames of
    // `size`. Returns the frame size arriving at the video node.
    ::android::base::Result<Size> Configure(uint32_t fourcc, Size size);

    // Sets the sensor's frame interval; returns the one applied.
    ::android::base::Result<Fraction> SetFrameInterval(const Fraction& interval);

    // The sensor, for exposure / white balance and other camera controls.
    ControlDevice* sensor() { return sensor_.get(); }

  private:
    PipelineController(std::shared_ptr<const MediaPipeline> pipeline, DeviceOpeners open,
                       std::unique_ptr<MediaDevice> media, std::unique_ptr<SubDevice> sensor);

    ::android::base::Result<void> EnableLinks();
    ::android::base::Result<SubDevice*> Stage(const std::string& path);

    const std::shared_ptr<const MediaPipeline> pipeline_;
    const DeviceOpeners open_;
    std::unique_ptr<MediaDevice> media_;
    std::unique_ptr<SubDevice> sensor_;
    // Intermediate stages, by device node, opened when first needed.
    std::map<std::string, std::unique_ptr<SubDevice>> stages_;
};

}  // namespace aidl::android::hardware::camera::mainline
