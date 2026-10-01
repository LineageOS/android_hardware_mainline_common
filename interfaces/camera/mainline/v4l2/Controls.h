/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>

namespace aidl::android::hardware::camera::mainline {

// Integer V4L2 controls, as offered by video nodes and sub-devices alike.
class ControlDevice {
  public:
    virtual ~ControlDevice() = default;

    // GetControl() returns nullopt when the control does not exist or can not
    // be read.
    virtual bool HasControl(uint32_t id) = 0;
    virtual std::optional<int32_t> GetControl(uint32_t id) = 0;
    virtual bool SetControl(uint32_t id, int32_t value) = 0;
};

// ioctl() retrying on EINTR.
int Xioctl(int fd, unsigned request, void* arg);

// The ControlDevice operations on a V4L2 file descriptor. `name` is for the
// log.
bool HasV4l2Control(int fd, uint32_t id);
std::optional<int32_t> GetV4l2Control(int fd, uint32_t id);
bool SetV4l2Control(int fd, const char* name, uint32_t id, int32_t value);

}  // namespace aidl::android::hardware::camera::mainline
