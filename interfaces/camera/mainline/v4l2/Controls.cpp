/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_V4l2"

#include "v4l2/Controls.h"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <android-base/logging.h>

namespace aidl::android::hardware::camera::mainline {

int Xioctl(int fd, unsigned request, void* arg) {
    return TEMP_FAILURE_RETRY(ioctl(fd, request, arg));
}

bool HasV4l2Control(int fd, uint32_t id) {
    v4l2_queryctrl query = {};
    query.id = id;
    return Xioctl(fd, VIDIOC_QUERYCTRL, &query) == 0 && !(query.flags & V4L2_CTRL_FLAG_DISABLED);
}

std::optional<int32_t> GetV4l2Control(int fd, uint32_t id) {
    v4l2_control control = {};
    control.id = id;
    if (Xioctl(fd, VIDIOC_G_CTRL, &control) != 0) return std::nullopt;
    return control.value;
}

bool SetV4l2Control(int fd, const char* name, uint32_t id, int32_t value) {
    v4l2_control control = {};
    control.id = id;
    control.value = value;
    if (Xioctl(fd, VIDIOC_S_CTRL, &control) != 0) {
        PLOG(WARNING) << name << ": failed to set control 0x" << std::hex << id << " to "
                      << std::dec << value;
        return false;
    }
    return true;
}

}  // namespace aidl::android::hardware::camera::mainline
