/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "tablet2multitouch"

#include "libtablet2multitouch.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <linux/input.h>
#include <sys/epoll.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using android::base::GetProperty;
using android::base::Split;
using android::base::unique_fd;
using tablet2multitouch::AbsInfo;
using tablet2multitouch::TabletEventTranslator;
using tablet2multitouch::UinputDevice;
using tablet2multitouch::UinputDeviceConfig;

constexpr char kDeviceNamesProp[] = "vendor.tablet2multitouch.device_names";
constexpr int kMaxDeviceNodes = 32;
constexpr int kEpollMaxEvents = 10;

constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;
constexpr size_t kEvMaxLongs = (EV_MAX + kBitsPerLong - 1) / kBitsPerLong;

bool TestBit(size_t bit, const unsigned long* array) {
    return (array[bit / kBitsPerLong] & (1UL << (bit % kBitsPerLong))) != 0;
}

struct DeviceInfo {
    unique_fd fd;
    std::string name;
    AbsInfo abs_x;
    AbsInfo abs_y;
};

std::vector<std::string> GetAllowedDeviceNames() {
    std::string names_str = GetProperty(kDeviceNamesProp, "");
    if (names_str.empty()) {
        return {};
    }
    return Split(names_str, ",");
}

std::optional<DeviceInfo> FindTabletDevice() {
    auto allowed_names = GetAllowedDeviceNames();
    if (allowed_names.empty()) {
        LOG(ERROR) << "Device names property not defined: " << kDeviceNamesProp;
        return std::nullopt;
    }

    for (int i = 0; i < kMaxDeviceNodes; ++i) {
        std::string path = "/dev/input/event" + std::to_string(i);
        unique_fd fd(open(path.c_str(), O_RDONLY | O_NONBLOCK));
        if (!fd.ok()) {
            continue;
        }

        char name_buf[64] = {};
        if (ioctl(fd.get(), EVIOCGNAME(sizeof(name_buf)), name_buf) < 0) {
            continue;
        }
        std::string device_name(name_buf);

        bool name_matches = false;
        for (const auto& allowed : allowed_names) {
            if (allowed == device_name) {
                name_matches = true;
                break;
            }
        }

        if (!name_matches) {
            LOG(INFO) << device_name << ": Device name mismatch";
            continue;
        }

        unsigned long ev_bits[kEvMaxLongs] = {};
        if (ioctl(fd.get(), EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
            continue;
        }

        if (!TestBit(EV_ABS, ev_bits) || !TestBit(EV_KEY, ev_bits)) {
            LOG(ERROR) << device_name << ": Device does not meet requirements";
            continue;
        }

        struct input_absinfo abs_x_info = {};
        struct input_absinfo abs_y_info = {};

        if (ioctl(fd.get(), EVIOCGABS(ABS_X), &abs_x_info) < 0) {
            PLOG(ERROR) << "ioctl EVIOCGABS(ABS_X) failed";
            continue;
        }

        if (ioctl(fd.get(), EVIOCGABS(ABS_Y), &abs_y_info) < 0) {
            PLOG(ERROR) << "ioctl EVIOCGABS(ABS_Y) failed";
            continue;
        }

        DeviceInfo info;
        info.fd = std::move(fd);
        info.name = device_name;
        info.abs_x.minimum = abs_x_info.minimum;
        info.abs_x.maximum = abs_x_info.maximum;
        info.abs_y.minimum = abs_y_info.minimum;
        info.abs_y.maximum = abs_y_info.maximum;

        return info;
    }

    return std::nullopt;
}

int RunEventLoop(unique_fd device_fd, TabletEventTranslator& translator) {
    unique_fd epoll_fd(epoll_create1(0));
    if (!epoll_fd.ok()) {
        PLOG(ERROR) << "epoll_create1() failed";
        return EXIT_FAILURE;
    }

    struct epoll_event event = {};
    event.events = EPOLLIN;
    event.data.fd = device_fd.get();

    if (epoll_ctl(epoll_fd.get(), EPOLL_CTL_ADD, device_fd.get(), &event) < 0) {
        PLOG(ERROR) << "epoll_ctl() failed";
        return EXIT_FAILURE;
    }

    struct epoll_event events[kEpollMaxEvents];
    while (true) {
        int num_events = epoll_wait(epoll_fd.get(), events, kEpollMaxEvents, -1);
        if (num_events < 0) {
            if (errno == EINTR) {
                continue;
            }
            PLOG(ERROR) << "epoll_wait() failed";
            return EXIT_FAILURE;
        }

        for (int i = 0; i < num_events; ++i) {
            if (!(events[i].events & EPOLLIN)) {
                continue;
            }

            struct input_event ev;
            ssize_t bytes_read = read(device_fd.get(), &ev, sizeof(ev));
            if (bytes_read == sizeof(ev)) {
                translator.HandleEvent(ev);
            } else if (bytes_read < 0 && errno != EAGAIN && errno != EINTR) {
                PLOG(ERROR) << "read() failed";
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__ANDROID_RECOVERY__)
    android::base::InitLogging(argv, &android::base::KernelLogger);
#endif

    auto device = FindTabletDevice();
    if (!device.has_value()) {
        LOG(ERROR) << "Tablet device not found";
        return EXIT_SUCCESS;
    }

    LOG(INFO) << "Using device: " << device->name;

#if defined(__ANDROID_RECOVERY__)
    constexpr bool kReportHover = false;
#else
    constexpr bool kReportHover = true;
#endif

    UinputDeviceConfig config;
    config.name = "tablet2multitouch";
    config.bustype = BUS_VIRTUAL;
    config.vendor = 0xCAFE;
    config.product = 0x0001;
    config.abs_x = device->abs_x;
    config.abs_y = device->abs_y;
    config.report_hover = kReportHover;

    UinputDevice uinput;
    if (!uinput.Create(config)) {
        LOG(ERROR) << "Failed to setup uinput device";
        return EXIT_FAILURE;
    }

    TabletEventTranslator translator(uinput, kReportHover);
    return RunEventLoop(std::move(device->fd), translator);
}
