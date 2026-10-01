/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Flash"

#include "flash/FlashLed.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <optional>

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include "utils/Sysfs.h"
#include "v4l2/Controls.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::android::base::ErrnoError;
using ::android::base::Error;
using ::android::base::Result;
using ::android::base::StartsWith;
using ::android::base::unique_fd;

class SysfsFlashLed : public FlashLed {
  public:
    SysfsFlashLed(std::string name, std::string dir, int32_t max_level)
        : name_(std::move(name)), dir_(std::move(dir)), max_level_(max_level) {}

    ~SysfsFlashLed() override { SetLevel(0); }

    const std::string& Name() const override { return name_; }
    int32_t MaxLevel() const override { return max_level_; }

    Result<void> SetLevel(int32_t level) override {
        const std::string path = dir_ + "/brightness";
        unique_fd fd(TEMP_FAILURE_RETRY(open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC)));
        if (fd.get() < 0) return ErrnoError() << "failed to open " << path;
        const std::string value = std::to_string(std::clamp(level, 0, max_level_));
        if (TEMP_FAILURE_RETRY(write(fd.get(), value.data(), value.size())) < 0) {
            return ErrnoError() << "failed to write " << value << " to " << path;
        }
        return {};
    }

  private:
    const std::string name_;
    const std::string dir_;
    const int32_t max_level_;
};

class V4l2FlashLed : public FlashLed {
  public:
    V4l2FlashLed(std::string devnode, unique_fd fd, std::optional<v4l2_queryctrl> intensity)
        : name_(std::move(devnode)), fd_(std::move(fd)), intensity_(intensity) {}

    ~V4l2FlashLed() override { SetLevel(0); }

    const std::string& Name() const override { return name_; }

    int32_t MaxLevel() const override {
        if (!intensity_.has_value()) return 1;
        const int32_t step = std::max(intensity_->step, 1);
        return std::max((intensity_->maximum - intensity_->minimum) / step + 1, 1);
    }

    Result<void> SetLevel(int32_t level) override {
        if (level <= 0) {
            if (!SetV4l2Control(fd_.get(), name_.c_str(), V4L2_CID_FLASH_LED_MODE,
                                V4L2_FLASH_LED_MODE_NONE)) {
                return Error(EIO) << name_ << ": failed to turn the LED off";
            }
            return {};
        }
        if (intensity_.has_value()) {
            const int32_t value =
                    std::min(intensity_->minimum + (std::min(level, MaxLevel()) - 1) *
                                                           std::max(intensity_->step, 1),
                             intensity_->maximum);
            SetV4l2Control(fd_.get(), name_.c_str(), V4L2_CID_FLASH_TORCH_INTENSITY, value);
        }
        if (!SetV4l2Control(fd_.get(), name_.c_str(), V4L2_CID_FLASH_LED_MODE,
                            V4L2_FLASH_LED_MODE_TORCH)) {
            return Error(EIO) << name_ << ": failed to turn the torch on";
        }
        return {};
    }

  private:
    const std::string name_;
    unique_fd fd_;
    const std::optional<v4l2_queryctrl> intensity_;
};

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

}  // namespace

Result<std::unique_ptr<FlashLed>> OpenSysfsFlashLed(const std::string& name,
                                                    const std::string& leds_dir) {
    if (name.empty() || name.find('/') != std::string::npos || name == "." || name == "..") {
        return Error(EINVAL) << "invalid LED name \"" << name << "\"";
    }
    const std::string dir = leds_dir + "/" + name;
    const auto max = ReadSysfsString(dir + "/max_brightness");
    if (!max.has_value()) return ErrnoError() << "failed to read " << dir << "/max_brightness";
    int32_t max_level;
    if (!::android::base::ParseInt(*max, &max_level, 1)) {
        return Error(EINVAL) << dir << ": invalid max_brightness \"" << *max << "\"";
    }
    return std::make_unique<SysfsFlashLed>(name, dir, max_level);
}

Result<std::unique_ptr<FlashLed>> OpenV4l2FlashLed(const std::string& devnode) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(devnode.c_str(), O_RDWR | O_CLOEXEC)));
    if (fd.get() < 0) return ErrnoError() << "failed to open " << devnode;
    if (!HasV4l2Control(fd.get(), V4L2_CID_FLASH_LED_MODE)) {
        return Error(ENOTSUP) << devnode << ": no V4L2_CID_FLASH_LED_MODE";
    }
    std::optional<v4l2_queryctrl> intensity;
    v4l2_queryctrl query = {};
    query.id = V4L2_CID_FLASH_TORCH_INTENSITY;
    if (Xioctl(fd.get(), VIDIOC_QUERYCTRL, &query) == 0 &&
        !(query.flags & V4L2_CTRL_FLAG_DISABLED) && query.maximum >= query.minimum) {
        intensity = query;
    }
    return std::make_unique<V4l2FlashLed>(devnode, std::move(fd), intensity);
}

Result<std::unique_ptr<FlashLed>> OpenFlashLed(const std::string& name,
                                               const std::string& leds_dir) {
    return StartsWith(name, "/dev/") ? OpenV4l2FlashLed(name) : OpenSysfsFlashLed(name, leds_dir);
}

bool IsFlashLedName(const std::string& name) {
    // The function follows the last ':' ("white:flash", "white:flash-1");
    // legacy names have it anywhere ("led:torch_0", "flashlight").
    const std::string lower = ToLower(name);
    return lower.find("flash") != std::string::npos || lower.find("torch") != std::string::npos;
}

std::vector<std::string> ListFlashLeds(const std::string& leds_dir) {
    std::vector<std::string> names;
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(leds_dir.c_str()), closedir);
    if (dir == nullptr) {
        if (errno != ENOENT) PLOG(DEBUG) << "failed to open " << leds_dir;
        return names;
    }
    while (const dirent* entry = readdir(dir.get())) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (IsFlashLedName(name)) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace aidl::android::hardware::camera::mainline
