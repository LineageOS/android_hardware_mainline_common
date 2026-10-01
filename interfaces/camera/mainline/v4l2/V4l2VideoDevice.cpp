/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_V4l2"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
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

    std::vector<FormatDescription> EnumerateFormats(uint32_t mbus_code) override;

    bool HasControl(uint32_t id) override;
    std::optional<int32_t> GetControl(uint32_t id) override;
    bool SetControl(uint32_t id, int32_t value) override;

    Result<CaptureFormat> SetFormat(uint32_t fourcc, uint32_t width, uint32_t height) override;
    Result<Fraction> SetFrameInterval(const Fraction& interval) override;
    Result<void> StartStreaming(uint32_t buffer_count) override;
    void StopStreaming() override;
    bool IsStreaming() const override { return streaming_; }
    Result<CapturedFrame> DequeueFrame(std::chrono::milliseconds timeout) override;
    Result<void> QueueFrame(uint32_t index) override;

    ~V4l2VideoDevice() override { StopStreaming(); }

  private:
    struct MappedPlane {
        void* address = MAP_FAILED;
        size_t length = 0;
    };
    using MappedBuffer = std::vector<MappedPlane>;

    // Fills the plane array of a v4l2_buffer for the multi-planar API.
    void PrepareBuffer(v4l2_buffer* buffer, v4l2_plane* planes, uint32_t index) const;
    void ReleaseBuffers();

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
    uint32_t num_planes_ = 1;
    std::vector<MappedBuffer> buffers_;
    bool streaming_ = false;
};

std::vector<FormatDescription> V4l2VideoDevice::EnumerateFormats(uint32_t mbus_code) {
    std::vector<FormatDescription> formats;
    for (uint32_t index = 0;; ++index) {
        v4l2_fmtdesc desc = {};
        desc.index = index;
        desc.type = BufferType();
        desc.mbus_code = mbus_code;
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
    return HasV4l2Control(fd_.get(), id);
}

std::optional<int32_t> V4l2VideoDevice::GetControl(uint32_t id) {
    return GetV4l2Control(fd_.get(), id);
}

bool V4l2VideoDevice::SetControl(uint32_t id, int32_t value) {
    return SetV4l2Control(fd_.get(), info_.name.c_str(), id, value);
}

Result<CaptureFormat> V4l2VideoDevice::SetFormat(uint32_t fourcc, uint32_t width, uint32_t height) {
    if (streaming_) return Error(EBUSY) << info_.name << ": can not set the format while streaming";

    v4l2_format format = {};
    format.type = BufferType();
    if (info_.multiplanar) {
        format.fmt.pix_mp.pixelformat = fourcc;
        format.fmt.pix_mp.width = width;
        format.fmt.pix_mp.height = height;
        format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    } else {
        format.fmt.pix.pixelformat = fourcc;
        format.fmt.pix.width = width;
        format.fmt.pix.height = height;
        format.fmt.pix.field = V4L2_FIELD_ANY;
    }
    if (Xioctl(fd_.get(), VIDIOC_S_FMT, &format) != 0) {
        return ErrnoError() << info_.name << ": VIDIOC_S_FMT " << FourccToString(fourcc) << " "
                            << width << "x" << height;
    }

    CaptureFormat result;
    if (info_.multiplanar) {
        const auto& pix = format.fmt.pix_mp;
        result.fourcc = pix.pixelformat;
        result.width = pix.width;
        result.height = pix.height;
        for (uint32_t i = 0; i < pix.num_planes && i < VIDEO_MAX_PLANES; ++i) {
            result.planes.push_back({pix.plane_fmt[i].bytesperline, pix.plane_fmt[i].sizeimage});
        }
    } else {
        const auto& pix = format.fmt.pix;
        result.fourcc = pix.pixelformat;
        result.width = pix.width;
        result.height = pix.height;
        result.planes.push_back({pix.bytesperline, pix.sizeimage});
    }
    if (result.fourcc != fourcc || result.width != width || result.height != height ||
        result.planes.empty()) {
        return Error(EINVAL) << info_.name << ": asked for " << FourccToString(fourcc) << " "
                             << width << "x" << height << ", got " << FourccToString(result.fourcc)
                             << " " << result.width << "x" << result.height;
    }
    num_planes_ = static_cast<uint32_t>(result.planes.size());
    LOG(DEBUG) << info_.name << ": format " << FourccToString(fourcc) << " " << width << "x"
               << height << ", " << num_planes_ << " plane(s), stride "
               << result.planes[0].bytes_per_line;
    return result;
}

Result<Fraction> V4l2VideoDevice::SetFrameInterval(const Fraction& interval) {
    v4l2_streamparm parm = {};
    parm.type = BufferType();
    if (Xioctl(fd_.get(), VIDIOC_G_PARM, &parm) != 0 ||
        !(parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        // The frame rate is not settable (e.g. an HDMI receiver).
        return Error(ENOTTY) << info_.name << ": frame interval not settable";
    }
    parm.parm.capture.timeperframe.numerator = interval.numerator;
    parm.parm.capture.timeperframe.denominator = interval.denominator;
    if (Xioctl(fd_.get(), VIDIOC_S_PARM, &parm) != 0) {
        return ErrnoError() << info_.name << ": VIDIOC_S_PARM";
    }
    return Fraction{parm.parm.capture.timeperframe.numerator,
                    parm.parm.capture.timeperframe.denominator};
}

void V4l2VideoDevice::PrepareBuffer(v4l2_buffer* buffer, v4l2_plane* planes, uint32_t index) const {
    *buffer = {};
    buffer->type = BufferType();
    buffer->memory = V4L2_MEMORY_MMAP;
    buffer->index = index;
    if (info_.multiplanar) {
        memset(planes, 0, sizeof(v4l2_plane) * VIDEO_MAX_PLANES);
        buffer->m.planes = planes;
        buffer->length = num_planes_;
    }
}

Result<void> V4l2VideoDevice::StartStreaming(uint32_t buffer_count) {
    if (streaming_) return {};

    v4l2_requestbuffers request = {};
    request.count = buffer_count;
    request.type = BufferType();
    request.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(fd_.get(), VIDIOC_REQBUFS, &request) != 0) {
        return ErrnoError() << info_.name << ": VIDIOC_REQBUFS " << buffer_count;
    }
    if (request.count == 0) return Error(ENOMEM) << info_.name << ": no buffers";

    for (uint32_t index = 0; index < request.count; ++index) {
        v4l2_buffer buffer;
        v4l2_plane planes[VIDEO_MAX_PLANES];
        PrepareBuffer(&buffer, planes, index);
        if (Xioctl(fd_.get(), VIDIOC_QUERYBUF, &buffer) != 0) {
            const int error = errno;
            ReleaseBuffers();
            return Error(error) << info_.name << ": VIDIOC_QUERYBUF " << index;
        }

        MappedBuffer mapped;
        for (uint32_t plane = 0; plane < (info_.multiplanar ? num_planes_ : 1); ++plane) {
            const size_t length = info_.multiplanar ? planes[plane].length : buffer.length;
            const off_t offset = info_.multiplanar ? planes[plane].m.mem_offset : buffer.m.offset;
            void* address =
                    mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), offset);
            if (address == MAP_FAILED) {
                const int error = errno;
                for (auto& done : mapped) munmap(done.address, done.length);
                ReleaseBuffers();
                return Error(error) << info_.name << ": mmap buffer " << index;
            }
            mapped.push_back({address, length});
        }
        buffers_.push_back(std::move(mapped));
    }

    for (uint32_t index = 0; index < buffers_.size(); ++index) {
        if (auto result = QueueFrame(index); !result.ok()) {
            ReleaseBuffers();
            return result.error();
        }
    }

    int type = static_cast<int>(BufferType());
    if (Xioctl(fd_.get(), VIDIOC_STREAMON, &type) != 0) {
        const int error = errno;
        ReleaseBuffers();
        return Error(error) << info_.name << ": VIDIOC_STREAMON";
    }
    streaming_ = true;
    LOG(DEBUG) << info_.name << ": streaming with " << buffers_.size() << " buffers";
    return {};
}

void V4l2VideoDevice::StopStreaming() {
    if (streaming_) {
        int type = static_cast<int>(BufferType());
        if (Xioctl(fd_.get(), VIDIOC_STREAMOFF, &type) != 0) {
            PLOG(WARNING) << info_.name << ": VIDIOC_STREAMOFF";
        }
        streaming_ = false;
        LOG(DEBUG) << info_.name << ": streaming stopped";
    }
    ReleaseBuffers();
}

void V4l2VideoDevice::ReleaseBuffers() {
    if (buffers_.empty()) return;
    for (auto& buffer : buffers_) {
        for (auto& plane : buffer) munmap(plane.address, plane.length);
    }
    buffers_.clear();
    v4l2_requestbuffers request = {};
    request.count = 0;
    request.type = BufferType();
    request.memory = V4L2_MEMORY_MMAP;
    if (Xioctl(fd_.get(), VIDIOC_REQBUFS, &request) != 0 && errno != ENODEV) {
        PLOG(WARNING) << info_.name << ": VIDIOC_REQBUFS 0";
    }
}

Result<CapturedFrame> V4l2VideoDevice::DequeueFrame(std::chrono::milliseconds timeout) {
    if (!streaming_) return Error(EINVAL) << info_.name << ": not streaming";

    pollfd pfd = {.fd = fd_.get(), .events = POLLIN, .revents = 0};
    const int ret = TEMP_FAILURE_RETRY(poll(&pfd, 1, static_cast<int>(timeout.count())));
    if (ret < 0) return ErrnoError() << info_.name << ": poll";
    if (ret == 0)
        return Error(ETIMEDOUT) << info_.name << ": no frame in " << timeout.count() << " ms";

    v4l2_buffer buffer;
    v4l2_plane planes[VIDEO_MAX_PLANES];
    PrepareBuffer(&buffer, planes, 0);
    if (Xioctl(fd_.get(), VIDIOC_DQBUF, &buffer) != 0) {
        return ErrnoError() << info_.name << ": VIDIOC_DQBUF";
    }
    if (buffer.index >= buffers_.size()) {
        return Error(EIO) << info_.name << ": bogus buffer index " << buffer.index;
    }

    CapturedFrame frame;
    frame.index = buffer.index;
    frame.sequence = buffer.sequence;
    frame.error = (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0;
    if ((buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
        frame.timestamp_ns = static_cast<int64_t>(buffer.timestamp.tv_sec) * 1'000'000'000LL +
                             static_cast<int64_t>(buffer.timestamp.tv_usec) * 1'000LL;
    }
    const MappedBuffer& mapped = buffers_[buffer.index];
    for (uint32_t plane = 0; plane < mapped.size(); ++plane) {
        const size_t used = info_.multiplanar ? planes[plane].bytesused : buffer.bytesused;
        const size_t offset = info_.multiplanar ? planes[plane].data_offset : 0;
        if (offset > used || used > mapped[plane].length) {
            frame.error = true;
            frame.planes.push_back({static_cast<const uint8_t*>(mapped[plane].address), 0});
            continue;
        }
        frame.planes.push_back(
                {static_cast<const uint8_t*>(mapped[plane].address) + offset, used - offset});
    }
    return frame;
}

Result<void> V4l2VideoDevice::QueueFrame(uint32_t index) {
    v4l2_buffer buffer;
    v4l2_plane planes[VIDEO_MAX_PLANES];
    PrepareBuffer(&buffer, planes, index);
    if (Xioctl(fd_.get(), VIDIOC_QBUF, &buffer) != 0) {
        return ErrnoError() << info_.name << ": VIDIOC_QBUF " << index;
    }
    return {};
}

void FillSysfsInfo(VideoDeviceInfo* info) {
    const std::string node = "/sys/class/video4linux/" + info->name;
    info->sysfs_device = CanonicalPath(node + "/device");
    if (info->sysfs_device.empty()) return;

    const std::string usb_device = FindSysfsAncestorWith(info->sysfs_device, "idVendor");
    if (usb_device.empty()) return;
    const auto vendor = ReadSysfsHex(usb_device + "/idVendor");
    const auto product = ReadSysfsHex(usb_device + "/idProduct");
    if (vendor.has_value() && product.has_value()) {
        info->usb_vendor_id = static_cast<uint16_t>(*vendor);
        info->usb_product_id = static_cast<uint16_t>(*product);
    }
    const auto removable = ReadSysfsString(usb_device + "/removable");
    if (removable == "removable") {
        info->usb_removable = true;
    } else if (removable == "fixed") {
        info->usb_removable = false;
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
