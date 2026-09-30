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
#include "session/DeviceControls.h"
#include "session/RequestSettings.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// The capture device of a session: streams in the capture mode of the
// current stream configuration and turns frames into I420 images.
class CaptureStream {
  public:
    explicit CaptureStream(std::unique_ptr<VideoDevice> device);
    ~CaptureStream();

    // Sets the capture mode for the next frames. Stops streaming; the next
    // Capture() starts again.
    void Configure(const CaptureMode& mode);

    // Applies the settings of a request, (re)starting streaming if needed
    // (e.g. the frame interval has to change).
    ::android::base::Result<void> Prepare(const RequestSettings& settings);

    // Waits for the next usable frame and converts it into `image`. Returns
    // its CLOCK_BOOTTIME timestamp. Frames the driver flags as corrupt or
    // that fail to decode are skipped. ENODEV means the device is gone.
    ::android::base::Result<int64_t> Capture(I420Image* image);

    void Stop();

    // Size of the captured frames.
    Size size() const { return mode_.size; }

  private:
    std::unique_ptr<VideoDevice> device_;
    DeviceControls controls_;
    CaptureMode mode_;
    std::optional<CaptureFormat> format_;
    Fraction interval_;
};

}  // namespace aidl::android::hardware::camera::mainline
