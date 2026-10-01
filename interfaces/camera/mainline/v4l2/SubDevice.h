/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <android-base/result.h>

#include "v4l2/Controls.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

// A media bus format on a sub-device pad.
struct MbusFormat {
    uint32_t code = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Whether a media bus code carries raw Bayer data.
bool IsBayerMbusCode(uint32_t code);

// A V4L2 sub-device node (/dev/v4l-subdevN), e.g. a camera sensor or a stage
// of a capture pipeline. Abstract for the unit tests.
class SubDevice : public ControlDevice {
  public:
    virtual const std::string& Path() const = 0;

    // Media bus codes a pad offers.
    virtual std::vector<uint32_t> EnumerateCodes(uint32_t pad) = 0;
    // Frame sizes of a pad for `code`, with the frame intervals the device
    // reports for them (shortest first; empty when it reports none).
    virtual std::vector<FrameSize> EnumerateSizes(uint32_t pad, uint32_t code) = 0;

    // Active format of a pad.
    virtual ::android::base::Result<MbusFormat> GetFormat(uint32_t pad) = 0;
    // Sets the active format of a pad and returns what the driver made of it.
    virtual ::android::base::Result<MbusFormat> SetFormat(uint32_t pad,
                                                          const MbusFormat& format) = 0;
    // Sets the frame interval of a (sensor's source) pad, returns the one
    // applied.
    virtual ::android::base::Result<Fraction> SetFrameInterval(uint32_t pad,
                                                               const Fraction& interval) = 0;
};

using SubDeviceOpener =
        std::function<::android::base::Result<std::unique_ptr<SubDevice>>(const std::string&)>;

::android::base::Result<std::unique_ptr<SubDevice>> OpenSubDevice(const std::string& path);

}  // namespace aidl::android::hardware::camera::mainline
