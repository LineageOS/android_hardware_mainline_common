/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_V4l2"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>

#include "utils/Sysfs.h"
#include "v4l2/PixelFormats.h"
#include "v4l2/VideoDevice.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::android::base::ErrnoError;
using ::android::base::Error;
using ::android::base::Result;
using ::android::base::unique_fd;

// Frame sizes offered for drivers that report a continuous / stepwise range
// instead of a list of discrete sizes.
constexpr struct {
    uint32_t width;
    uint32_t height;
} kStandardSizes[] = {
        {160, 120},   {176, 144},   {320, 240},   {352, 288},   {640, 360},
        {640, 480},   {800, 600},   {1024, 768},  {1280, 720},  {1280, 960},
        {1600, 1200}, {1920, 1080}, {2560, 1440}, {3840, 2160},
};

// Frame rates offered for a continuous / stepwise frame interval range.
constexpr uint32_t kStandardFrameRates[] = {60, 30, 15};

// Frame interval assumed when the driver does not report any.
constexpr Fraction kDefaultInterval = {1, 30};

int Xioctl(int fd, unsigned long request, void* arg) {
    return TEMP_FAILURE_RETRY(ioctl(fd, request, arg));
}

std::string CString(const uint8_t* data, size_t size) {
    const char* chars = reinterpret_cast<const char*>(data);
    return std::string(chars, strnlen(chars, size));
}

// a/b < c/d
bool IntervalLess(const Fraction& a, const Fraction& b) {
    return static_cast<uint64_t>(a.numerator) * b.denominator <
           static_cast<uint64_t>(b.numerator) * a.denominator;
}

bool InStepRange(uint32_t value, uint32_t min, uint32_t max, uint32_t step) {
    if (value < min || value > max) return false;
    return step <= 1 || (value - min) % step == 0;
}

class V4l2VideoDevice : public VideoDevice {
  public:
    V4l2VideoDevice(unique_fd fd, VideoDeviceInfo info)
        : fd_(std::move(fd)), info_(std::move(info)) {}

    const VideoDeviceInfo& Info() const override { return info_; }

    std::vector<FormatDescription> EnumerateFormats() override;

    bool HasControl(uint32_t id) override;
    std::optional<int32_t> GetControl(uint32_t id) override;
    bool SetControl(uint32_t id, int32_t value) override;

  private:
    uint32_t BufferType() const {
        return info_.multiplanar ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    }

    std::vector<FrameSize> EnumerateSizes(uint32_t fourcc);
    std::vector<Fraction> EnumerateIntervals(uint32_t fourcc, uint32_t width, uint32_t height);
    // Size the driver picks for `fourcc` when asked for a huge frame, i.e.
    // its maximum. Used for drivers without VIDIOC_ENUM_FRAMESIZES.
    std::optional<FrameSize> TryMaximumSize(uint32_t fourcc);

    unique_fd fd_;
    VideoDeviceInfo info_;
};

std::vector<FormatDescription> V4l2VideoDevice::EnumerateFormats() {
    std::vector<FormatDescription> formats;
    for (uint32_t index = 0;; ++index) {
        v4l2_fmtdesc desc = {};
        desc.index = index;
        desc.type = BufferType();
        if (Xioctl(fd_.get(), VIDIOC_ENUM_FMT, &desc) != 0) break;

        FormatDescription format;
        format.fourcc = desc.pixelformat;
        format.description = CString(desc.description, sizeof(desc.description));
        format.emulated = (desc.flags & V4L2_FMT_FLAG_EMULATED) != 0;
        format.sizes = EnumerateSizes(desc.pixelformat);
        LOG(DEBUG) << info_.name << ": format " << FourccToString(format.fourcc) << " ("
                   << format.description << "), " << format.sizes.size() << " size(s)";
        formats.push_back(std::move(format));
    }
    return formats;
}

std::vector<FrameSize> V4l2VideoDevice::EnumerateSizes(uint32_t fourcc) {
    std::vector<FrameSize> sizes;
    for (uint32_t index = 0;; ++index) {
        v4l2_frmsizeenum frmsize = {};
        frmsize.index = index;
        frmsize.pixel_format = fourcc;
        if (Xioctl(fd_.get(), VIDIOC_ENUM_FRAMESIZES, &frmsize) != 0) break;

        if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            sizes.push_back({frmsize.discrete.width, frmsize.discrete.height, {}});
            continue;
        }

        // Stepwise or continuous: the only entry, offer the standard sizes
        // that fit plus the maximum.
        const auto& sw = frmsize.stepwise;
        for (const auto& standard : kStandardSizes) {
            if (InStepRange(standard.width, sw.min_width, sw.max_width, sw.step_width) &&
                InStepRange(standard.height, sw.min_height, sw.max_height, sw.step_height)) {
                sizes.push_back({standard.width, standard.height, {}});
            }
        }
        sizes.push_back({sw.max_width, sw.max_height, {}});
        break;
    }

    if (sizes.empty()) {
        // e.g. HDMI receivers, whose size follows the incoming signal.
        if (auto size = TryMaximumSize(fourcc); size.has_value()) sizes.push_back(*size);
    }

    for (auto& size : sizes) size.intervals = EnumerateIntervals(fourcc, size.width, size.height);

    // Largest first, without duplicates.
    std::sort(sizes.begin(), sizes.end(), [](const FrameSize& a, const FrameSize& b) {
        return a.Area() != b.Area() ? a.Area() > b.Area() : a.width > b.width;
    });
    sizes.erase(std::unique(sizes.begin(), sizes.end(),
                            [](const FrameSize& a, const FrameSize& b) {
                                return a.width == b.width && a.height == b.height;
                            }),
                sizes.end());
    return sizes;
}

std::vector<Fraction> V4l2VideoDevice::EnumerateIntervals(uint32_t fourcc, uint32_t width,
                                                          uint32_t height) {
    std::vector<Fraction> intervals;
    for (uint32_t index = 0;; ++index) {
        v4l2_frmivalenum frmival = {};
        frmival.index = index;
        frmival.pixel_format = fourcc;
        frmival.width = width;
        frmival.height = height;
        if (Xioctl(fd_.get(), VIDIOC_ENUM_FRAMEINTERVALS, &frmival) != 0) break;

        if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            if (frmival.discrete.numerator != 0 && frmival.discrete.denominator != 0) {
                intervals.push_back({frmival.discrete.numerator, frmival.discrete.denominator});
            }
            continue;
        }

        // Stepwise or continuous: the shortest interval plus the standard
        // rates within the range.
        const auto& sw = frmival.stepwise;
        const Fraction min = {sw.min.numerator, sw.min.denominator};
        const Fraction max = {sw.max.numerator, sw.max.denominator};
        if (min.numerator != 0 && min.denominator != 0) intervals.push_back(min);
        for (const uint32_t rate : kStandardFrameRates) {
            const Fraction interval = {1, rate};
            if (!IntervalLess(interval, min) && !IntervalLess(max, interval)) {
                intervals.push_back(interval);
            }
        }
        break;
    }

    if (intervals.empty()) {
        v4l2_streamparm parm = {};
        parm.type = BufferType();
        if (Xioctl(fd_.get(), VIDIOC_G_PARM, &parm) == 0 &&
            (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) &&
            parm.parm.capture.timeperframe.numerator != 0 &&
            parm.parm.capture.timeperframe.denominator != 0) {
            intervals.push_back({parm.parm.capture.timeperframe.numerator,
                                 parm.parm.capture.timeperframe.denominator});
        } else {
            intervals.push_back(kDefaultInterval);
        }
    }

    std::sort(intervals.begin(), intervals.end(), IntervalLess);
    intervals.erase(std::unique(intervals.begin(), intervals.end(),
                                [](const Fraction& a, const Fraction& b) {
                                    return !IntervalLess(a, b) && !IntervalLess(b, a);
                                }),
                    intervals.end());
    return intervals;
}

std::optional<FrameSize> V4l2VideoDevice::TryMaximumSize(uint32_t fourcc) {
    v4l2_format format = {};
    format.type = BufferType();
    if (info_.multiplanar) {
        format.fmt.pix_mp.pixelformat = fourcc;
        format.fmt.pix_mp.width = 16384;
        format.fmt.pix_mp.height = 16384;
    } else {
        format.fmt.pix.pixelformat = fourcc;
        format.fmt.pix.width = 16384;
        format.fmt.pix.height = 16384;
    }
    if (Xioctl(fd_.get(), VIDIOC_TRY_FMT, &format) != 0) {
        PLOG(DEBUG) << info_.name << ": VIDIOC_TRY_FMT " << FourccToString(fourcc);
        return std::nullopt;
    }
    const uint32_t width = info_.multiplanar ? format.fmt.pix_mp.width : format.fmt.pix.width;
    const uint32_t height = info_.multiplanar ? format.fmt.pix_mp.height : format.fmt.pix.height;
    const uint32_t result_fourcc =
            info_.multiplanar ? format.fmt.pix_mp.pixelformat : format.fmt.pix.pixelformat;
    if (result_fourcc != fourcc || width == 0 || height == 0) return std::nullopt;
    return FrameSize{width, height, {}};
}

bool V4l2VideoDevice::HasControl(uint32_t id) {
    v4l2_queryctrl query = {};
    query.id = id;
    return Xioctl(fd_.get(), VIDIOC_QUERYCTRL, &query) == 0 &&
           !(query.flags & V4L2_CTRL_FLAG_DISABLED);
}

std::optional<int32_t> V4l2VideoDevice::GetControl(uint32_t id) {
    v4l2_control control = {};
    control.id = id;
    if (Xioctl(fd_.get(), VIDIOC_G_CTRL, &control) != 0) return std::nullopt;
    return control.value;
}

bool V4l2VideoDevice::SetControl(uint32_t id, int32_t value) {
    v4l2_control control = {};
    control.id = id;
    control.value = value;
    if (Xioctl(fd_.get(), VIDIOC_S_CTRL, &control) != 0) {
        PLOG(WARNING) << info_.name << ": failed to set control 0x" << std::hex << id << " to "
                      << std::dec << value;
        return false;
    }
    return true;
}

void FillSysfsInfo(VideoDeviceInfo* info) {
    info->sysfs_device = CanonicalPath("/sys/class/video4linux/" + info->name + "/device");
    if (info->sysfs_device.empty()) return;

    const std::string usb_device = FindSysfsAncestorWith(info->sysfs_device, "idVendor");
    if (usb_device.empty()) return;
    const auto vendor = ReadSysfsHex(usb_device + "/idVendor");
    const auto product = ReadSysfsHex(usb_device + "/idProduct");
    if (vendor.has_value() && product.has_value()) {
        info->usb_vendor_id = static_cast<uint16_t>(*vendor);
        info->usb_product_id = static_cast<uint16_t>(*product);
    }
}

}  // namespace

int64_t Fraction::ToNanoseconds() const {
    if (numerator == 0 || denominator == 0) return 0;
    return static_cast<int64_t>(numerator) * 1'000'000'000LL / denominator;
}

Result<std::unique_ptr<VideoDevice>> OpenVideoDevice(const std::string& path) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC)));
    if (fd.get() < 0) return ErrnoError() << "open " << path;

    struct stat st;
    if (fstat(fd.get(), &st) != 0) return ErrnoError() << "fstat " << path;
    if (!S_ISCHR(st.st_mode)) return Error(ENODEV) << path << " is not a character device";

    v4l2_capability cap = {};
    if (Xioctl(fd.get(), VIDIOC_QUERYCAP, &cap) != 0) return ErrnoError() << "VIDIOC_QUERYCAP";

    VideoDeviceInfo info;
    info.path = path;
    info.name = path.substr(path.rfind('/') + 1);
    info.rdev = st.st_rdev;
    info.driver = CString(cap.driver, sizeof(cap.driver));
    info.card = CString(cap.card, sizeof(cap.card));
    info.bus_info = CString(cap.bus_info, sizeof(cap.bus_info));
    info.device_caps =
            (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    info.multiplanar = !(info.device_caps & V4L2_CAP_VIDEO_CAPTURE) &&
                       (info.device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE);
    FillSysfsInfo(&info);

    std::unique_ptr<VideoDevice> device =
            std::make_unique<V4l2VideoDevice>(std::move(fd), std::move(info));
    return device;
}

}  // namespace aidl::android::hardware::camera::mainline
