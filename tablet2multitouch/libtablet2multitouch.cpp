/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "libtablet2multitouch"

#include "libtablet2multitouch.h"

#include <android-base/logging.h>
#include <android-base/unique_fd.h>

#include <linux/input.h>
#include <linux/uinput.h>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

namespace tablet2multitouch {

using android::base::unique_fd;

UinputDevice::~UinputDevice() {
    Destroy();
}

UinputDevice::UinputDevice(UinputDevice&& other) noexcept
    : fd_(other.fd_), created_(other.created_) {
    other.fd_ = -1;
    other.created_ = false;
}

UinputDevice& UinputDevice::operator=(UinputDevice&& other) noexcept {
    if (this != &other) {
        Destroy();
        fd_ = other.fd_;
        created_ = other.created_;
        other.fd_ = -1;
        other.created_ = false;
    }
    return *this;
}

bool UinputDevice::Create(const UinputDeviceConfig& config) {
    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        PLOG(ERROR) << "Failed to open /dev/uinput";
        return false;
    }

    if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) {
        PLOG(ERROR) << "ioctl(UI_SET_EVBIT, EV_KEY) failed";
        return false;
    }

    static const uint16_t kKeyBits[] = {
            BTN_TOUCH, KEY_BACK, KEY_MENU, KEY_UP, KEY_DOWN, KEY_VOLUMEUP, KEY_VOLUMEDOWN,
    };
    for (uint16_t key : kKeyBits) {
        if (ioctl(fd_, UI_SET_KEYBIT, key) < 0) {
            PLOG(ERROR) << "ioctl(UI_SET_KEYBIT, " << key << ") failed";
            return false;
        }
    }

    if (ioctl(fd_, UI_SET_EVBIT, EV_ABS) < 0) {
        PLOG(ERROR) << "ioctl(UI_SET_EVBIT, EV_ABS) failed";
        return false;
    }

    static const uint16_t kAbsBits[] = {
            ABS_X,
            ABS_Y,
            ABS_MT_SLOT,
            ABS_MT_POSITION_X,
            ABS_MT_POSITION_Y,
            ABS_MT_TRACKING_ID,
            ABS_MT_TOOL_TYPE,
            ABS_MT_DISTANCE,
    };
    for (uint16_t abs : kAbsBits) {
        if (ioctl(fd_, UI_SET_ABSBIT, abs) < 0) {
            PLOG(ERROR) << "ioctl(UI_SET_ABSBIT, " << abs << ") failed";
            return false;
        }
    }

    if (ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
        PLOG(ERROR) << "ioctl(UI_SET_PROPBIT, INPUT_PROP_DIRECT) failed";
        return false;
    }

    struct uinput_abs_setup abs_setup = {};

    abs_setup.code = ABS_MT_SLOT;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 1;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_MT_SLOT) failed";
        return false;
    }

    abs_setup.code = ABS_X;
    abs_setup.absinfo.minimum = config.abs_x.minimum;
    abs_setup.absinfo.maximum = config.abs_x.maximum;
    abs_setup.absinfo.value = 0;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_X) failed";
        return false;
    }

    abs_setup.code = ABS_MT_POSITION_X;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_MT_POSITION_X) failed";
        return false;
    }

    abs_setup.code = ABS_Y;
    abs_setup.absinfo.minimum = config.abs_y.minimum;
    abs_setup.absinfo.maximum = config.abs_y.maximum;
    abs_setup.absinfo.value = 0;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_Y) failed";
        return false;
    }

    abs_setup.code = ABS_MT_POSITION_Y;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_MT_POSITION_Y) failed";
        return false;
    }

    abs_setup.code = ABS_MT_TRACKING_ID;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = TRKID_MAX;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_MT_TRACKING_ID) failed";
        return false;
    }

    abs_setup.code = ABS_MT_TOOL_TYPE;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = MT_TOOL_PEN;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_MT_TOOL_TYPE) failed";
        return false;
    }

    abs_setup.code = ABS_MT_DISTANCE;
    abs_setup.absinfo.minimum = 0;
    abs_setup.absinfo.maximum = 1;
    if (ioctl(fd_, UI_ABS_SETUP, &abs_setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_ABS_SETUP, ABS_MT_DISTANCE) failed";
        return false;
    }

    struct uinput_setup setup = {};
    setup.id.bustype = config.bustype;
    setup.id.vendor = config.vendor;
    setup.id.product = config.product;
    strncpy(setup.name, config.name, UINPUT_MAX_NAME_SIZE - 1);

    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_DEV_SETUP) failed";
        return false;
    }

    if (ioctl(fd_, UI_DEV_CREATE) < 0) {
        PLOG(ERROR) << "ioctl(UI_DEV_CREATE) failed";
        return false;
    }

    created_ = true;
    return true;
}

void UinputDevice::Destroy() {
    if (created_ && fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        created_ = false;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void UinputDevice::SendEvent(uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev = {};
    ev.type = type;
    ev.code = code;
    ev.value = value;

    if (write(fd_, &ev, sizeof(ev)) < 0) {
        PLOG(ERROR) << "Failed to write input event";
    }
}

void UinputDevice::ReportSync() {
    SendEvent(EV_SYN, SYN_REPORT, 0);
}

void UinputDevice::ReportKey(uint16_t code, int32_t value) {
    SendEvent(EV_KEY, code, value);
}

void UinputDevice::ReportMultitouch(bool active, bool contact, int tracking_id, int32_t x,
                                    int32_t y) {
    SendEvent(EV_ABS, ABS_MT_SLOT, 0);

    if (active) {
        SendEvent(EV_ABS, ABS_MT_TRACKING_ID, tracking_id);
        SendEvent(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PEN);
        SendEvent(EV_ABS, ABS_MT_POSITION_X, x);
        SendEvent(EV_ABS, ABS_MT_POSITION_Y, y);
        SendEvent(EV_ABS, ABS_MT_DISTANCE, contact ? 0 : 1);
        SendEvent(EV_KEY, BTN_TOUCH, contact ? 1 : 0);
        SendEvent(EV_ABS, ABS_X, x);
        SendEvent(EV_ABS, ABS_Y, y);
    } else {
        SendEvent(EV_ABS, ABS_MT_TRACKING_ID, -1);
        SendEvent(EV_KEY, BTN_TOUCH, 0);
    }
}

MouseUinputDevice::~MouseUinputDevice() {
    Destroy();
}

MouseUinputDevice::MouseUinputDevice(MouseUinputDevice&& other) noexcept
    : fd_(other.fd_), created_(other.created_) {
    other.fd_ = -1;
    other.created_ = false;
}

MouseUinputDevice& MouseUinputDevice::operator=(MouseUinputDevice&& other) noexcept {
    if (this != &other) {
        Destroy();
        fd_ = other.fd_;
        created_ = other.created_;
        other.fd_ = -1;
        other.created_ = false;
    }
    return *this;
}

bool MouseUinputDevice::Create(const MouseUinputDeviceConfig& config) {
    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd_ < 0) {
        PLOG(ERROR) << "Failed to open /dev/uinput";
        return false;
    }

    if (ioctl(fd_, UI_SET_EVBIT, EV_KEY) < 0) {
        PLOG(ERROR) << "ioctl(UI_SET_EVBIT, EV_KEY) failed";
        return false;
    }

    static const uint16_t kKeyBits[] = {
            BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA,
    };
    for (uint16_t key : kKeyBits) {
        if (ioctl(fd_, UI_SET_KEYBIT, key) < 0) {
            PLOG(ERROR) << "ioctl(UI_SET_KEYBIT, " << key << ") failed";
            return false;
        }
    }

    if (ioctl(fd_, UI_SET_EVBIT, EV_REL) < 0) {
        PLOG(ERROR) << "ioctl(UI_SET_EVBIT, EV_REL) failed";
        return false;
    }

    static const uint16_t kRelBits[] = {
            REL_X,
            REL_Y,
            REL_HWHEEL,
            REL_WHEEL,
    };
    for (uint16_t rel : kRelBits) {
        if (ioctl(fd_, UI_SET_RELBIT, rel) < 0) {
            PLOG(ERROR) << "ioctl(UI_SET_RELBIT, " << rel << ") failed";
            return false;
        }
    }

    if (ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_POINTER) < 0) {
        PLOG(ERROR) << "ioctl(UI_SET_PROPBIT, INPUT_PROP_POINTER) failed";
        return false;
    }

    struct uinput_setup setup = {};
    setup.id.bustype = config.bustype;
    setup.id.vendor = config.vendor;
    setup.id.product = config.product;
    strncpy(setup.name, config.name, UINPUT_MAX_NAME_SIZE - 1);

    if (ioctl(fd_, UI_DEV_SETUP, &setup) < 0) {
        PLOG(ERROR) << "ioctl(UI_DEV_SETUP) failed";
        return false;
    }

    if (ioctl(fd_, UI_DEV_CREATE) < 0) {
        PLOG(ERROR) << "ioctl(UI_DEV_CREATE) failed";
        return false;
    }

    created_ = true;
    return true;
}

void MouseUinputDevice::Destroy() {
    if (created_ && fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        created_ = false;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void MouseUinputDevice::ForwardEvent(const struct input_event& ev) {
    if (write(fd_, &ev, sizeof(ev)) < 0) {
        PLOG(ERROR) << "Failed to forward mouse input event";
    }
}

TabletEventTranslator::TabletEventTranslator(UinputDevice& uinput, bool report_hover)
    : uinput_(uinput), report_hover_(report_hover) {
    if (report_hover_) {
        active_ = true;
    } else {
        contact_ = true;
    }
}

void TabletEventTranslator::HandleEvent(const struct input_event& ev) {
    switch (ev.type) {
        case EV_KEY:
            ProcessKeyEvent(ev.code, ev.value);
            break;
        case EV_REL:
            ProcessRelEvent(ev.code, ev.value);
            break;
        case EV_ABS:
            ProcessAbsEvent(ev.code, ev.value);
            break;
        case EV_SYN:
            ProcessSynEvent(ev.code);
            break;
    }
}

void TabletEventTranslator::ProcessKeyEvent(uint16_t code, int32_t value) {
    if (code == BTN_LEFT) {
        if (report_hover_) {
            contact_ = (value != 0);
        } else {
            active_ = (value != 0);
        }
        pending_multitouch_ = true;
        return;
    }

    uint16_t trans_keycode;
    bool key_report_up = false;

    switch (code) {
        case BTN_MIDDLE:
            trans_keycode = KEY_MENU;
            break;
        case BTN_RIGHT:
            trans_keycode = KEY_BACK;
            break;
        case BTN_GEAR_DOWN:
            key_report_up = true;
            trans_keycode = KEY_DOWN;
            break;
        case BTN_GEAR_UP:
            key_report_up = true;
            trans_keycode = KEY_UP;
            break;
        default:
            return;
    }

    if (key_report_up) {
        uinput_.ReportKey(trans_keycode, 1);
        uinput_.ReportKey(trans_keycode, 0);
    } else {
        uinput_.ReportKey(trans_keycode, value);
    }
    pending_report_ = true;
}

void TabletEventTranslator::ProcessRelEvent(uint16_t code, int32_t value) {
    if (code != REL_WHEEL) {
        return;
    }

    uint16_t trans_keycode = (value == 1) ? KEY_VOLUMEUP : KEY_VOLUMEDOWN;
    uinput_.ReportKey(trans_keycode, 1);
    uinput_.ReportKey(trans_keycode, 0);
    pending_report_ = true;
}

void TabletEventTranslator::ProcessAbsEvent(uint16_t code, int32_t value) {
    switch (code) {
        case ABS_X:
            x_ = value;
            break;
        case ABS_Y:
            y_ = value;
            break;
        default:
            return;
    }
    pending_multitouch_ = true;
}

void TabletEventTranslator::ProcessSynEvent(uint16_t code) {
    if (code != SYN_REPORT) {
        return;
    }

    if (pending_multitouch_ && !(prev_active_ == false && active_ == false)) {
        pending_report_ = true;
        uinput_.ReportMultitouch(active_, contact_, tracking_id_, x_, y_);

        if (!active_) {
            tracking_id_++;
            if (tracking_id_ > TRKID_MAX) {
                tracking_id_ = 0;
            }
        }

        pending_multitouch_ = false;
        prev_active_ = active_;
    }

    if (pending_report_) {
        uinput_.ReportSync();
        pending_report_ = false;
    }
}

}  // namespace tablet2multitouch
