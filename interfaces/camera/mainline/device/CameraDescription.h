/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <aidl/android/hardware/camera/device/StreamConfiguration.h>

#include "device/StreamPlanner.h"
#include "provider/Discovery.h"
#include "utils/Metadata.h"

namespace aidl::android::hardware::camera::mainline {

// Maximum number of output streams per session, by kind
// (ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS): no RAW, two processed (YUV /
// PRIVATE) and one stalling (JPEG).
constexpr int kMaxProcessedStreams = 2;
constexpr int kMaxStallingStreams = 1;

// Frames that can be in flight in the pipeline
// (ANDROID_REQUEST_PIPELINE_MAX_DEPTH).
constexpr uint8_t kPipelineMaxDepth = 4;

// Everything about a camera that is fixed once it is discovered, shared by
// the camera device, its sessions and the request templates.
class CameraDescription {
  public:
    static std::shared_ptr<const CameraDescription> Create(const CameraCandidate& candidate);

    const CameraCandidate& candidate() const { return candidate_; }
    const StreamPlanner& planner() const { return planner_; }
    const Metadata& characteristics() const { return characteristics_; }

    bool internal() const { return candidate_.internal; }
    // The full field of view: ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE.
    Size active_array() const { return planner_.MaxSize(); }
    float max_zoom() const { return max_zoom_; }
    // ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES as (min, max) pairs.
    const std::vector<std::array<int32_t, 2>>& fps_ranges() const { return fps_ranges_; }
    int32_t jpeg_max_size() const { return jpeg_max_size_; }
    float focal_length() const { return focal_length_; }
    float aperture() const { return aperture_; }

    // Checks a stream configuration and picks the capture mode for it.
    // Returns nullopt, with the reason in `why`, if it is not supported.
    std::optional<CaptureMode> PlanStreams(const device::StreamConfiguration& config,
                                           std::string* why) const;

  private:
    explicit CameraDescription(const CameraCandidate& candidate);
    void BuildCharacteristics();

    CameraCandidate candidate_;
    StreamPlanner planner_;
    float max_zoom_ = 1.0f;
    std::vector<std::array<int32_t, 2>> fps_ranges_;
    int32_t jpeg_max_size_ = 0;
    float focal_length_ = 0.0f;
    float aperture_ = 0.0f;
    Metadata characteristics_;
};

}  // namespace aidl::android::hardware::camera::mainline
