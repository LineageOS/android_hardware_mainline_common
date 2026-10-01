/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>

#include <android-base/result.h>

#include "convert/Image.h"
#include "device/StreamPlanner.h"
#include "isp/Isp3A.h"
#include "isp/SoftIsp.h"
#include "session/DeviceControls.h"
#include "session/PipelineController.h"
#include "session/RequestSettings.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// The capture device of a session: streams in the capture mode of the
// current stream configuration and turns frames into I420 images.
class CaptureStream {
  public:
    // `pipeline` is set for a sensor behind a media controller; the pipeline
    // is configured before the video node, and the sensor gets the camera
    // controls. Raw Bayer capture modes go through the software ISP, with
    // `black_level` (see SoftIsp).
    CaptureStream(std::unique_ptr<VideoDevice> device,
                  std::unique_ptr<PipelineController> pipeline = nullptr,
                  std::optional<int> black_level = std::nullopt);
    ~CaptureStream();

    // Sets the capture mode for the next frames. Stops streaming; the next
    // Capture() starts again.
    void Configure(const CaptureMode& mode);

    // Applies the settings of a request, (re)starting streaming if needed
    // (e.g. the frame interval has to change).
    ::android::base::Result<void> Prepare(const RequestSettings& settings);

    // Waits for the next usable frame and converts it into `image`. Returns
    // its CLOCK_BOOTTIME timestamp. Frames the driver flags as corrupt or
    // that fail to decode are skipped, and so are frames that started before
    // `not_before_ns` (CLOCK_MONOTONIC, e.g. before the flash came on).
    // ENODEV means the device is gone.
    ::android::base::Result<int64_t> Capture(I420Image* image, int64_t not_before_ns = 0);

    void Stop();

    // Size of the captured frames.
    Size size() const { return frame_size_; }

  private:
    std::unique_ptr<VideoDevice> device_;
    std::unique_ptr<PipelineController> pipeline_;
    ControlDevice* const sensor_;
    DeviceControls controls_;
    // For raw Bayer capture modes.
    SoftIsp isp_;
    Isp3A isp_3a_;
    bool raw_ = false;
    // Differs from the mode's size when a pipeline stage scales.
    Size frame_size_;
    CaptureMode mode_;
    std::optional<CaptureFormat> format_;
    Fraction interval_;
};

}  // namespace aidl::android::hardware::camera::mainline
