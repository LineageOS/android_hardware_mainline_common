/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <linux/input.h>
#include <linux/uinput.h>

#include <cstdint>

#ifndef TRKID_MAX
#define TRKID_MAX 0xffff
#endif

namespace tablet2multitouch {

struct AbsInfo {
    int32_t minimum;
    int32_t maximum;
};

struct UinputDeviceConfig {
    const char* name;
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    AbsInfo abs_x;
    AbsInfo abs_y;
    bool report_hover;
};

class UinputDevice {
  public:
    UinputDevice() = default;
    ~UinputDevice();

    UinputDevice(const UinputDevice&) = delete;
    UinputDevice& operator=(const UinputDevice&) = delete;
    UinputDevice(UinputDevice&& other) noexcept;
    UinputDevice& operator=(UinputDevice&& other) noexcept;

    bool Create(const UinputDeviceConfig& config);
    void Destroy();

    void SendEvent(uint16_t type, uint16_t code, int32_t value);
    void ReportSync();
    void ReportKey(uint16_t code, int32_t value);
    void ReportMultitouch(bool active, bool contact, int tracking_id, int32_t x, int32_t y);

    int GetFd() const { return fd_; }
    bool IsValid() const { return fd_ >= 0; }

  private:
    int fd_ = -1;
    bool created_ = false;
};

struct MouseUinputDeviceConfig {
    const char* name;
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
};

class MouseUinputDevice {
  public:
    MouseUinputDevice() = default;
    ~MouseUinputDevice();

    MouseUinputDevice(const MouseUinputDevice&) = delete;
    MouseUinputDevice& operator=(const MouseUinputDevice&) = delete;
    MouseUinputDevice(MouseUinputDevice&& other) noexcept;
    MouseUinputDevice& operator=(MouseUinputDevice&& other) noexcept;

    bool Create(const MouseUinputDeviceConfig& config);
    void Destroy();

    void ForwardEvent(const struct input_event& ev);

    int GetFd() const { return fd_; }
    bool IsValid() const { return fd_ >= 0; }

  private:
    int fd_ = -1;
    bool created_ = false;
};

class TabletEventTranslator {
  public:
    explicit TabletEventTranslator(UinputDevice& uinput, bool report_hover);

    void HandleEvent(const struct input_event& ev);

  private:
    void ProcessKeyEvent(uint16_t code, int32_t value);
    void ProcessRelEvent(uint16_t code, int32_t value);
    void ProcessAbsEvent(uint16_t code, int32_t value);
    void ProcessSynEvent(uint16_t code);

    UinputDevice& uinput_;
    bool report_hover_;

    bool active_ = false;
    bool contact_ = false;
    bool prev_active_ = false;
    bool pending_report_ = false;
    bool pending_multitouch_ = false;
    int tracking_id_ = 0;
    int32_t x_ = 0;
    int32_t y_ = 0;
};

}  // namespace tablet2multitouch
