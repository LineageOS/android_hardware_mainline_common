/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "ts_vkeys"

#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/epoll.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using android::base::GetProperty;
using android::base::GetUintProperty;
using android::base::Split;
using android::base::unique_fd;

constexpr char kPropPrefix[] = "vendor.ts_vkeys.";
constexpr int kDeviceSearchRounds = 10;
constexpr int kDeviceSearchMaxNodes = 10;
constexpr int kEpollMaxEvents = 10;

struct TouchSlot {
    bool active = false;
    int32_t x = 0;
    int32_t y = 0;
    uint16_t key_code = 0;
};

class UinputDevice {
  public:
    UinputDevice() = default;
    ~UinputDevice() { Destroy(); }

    UinputDevice(const UinputDevice&) = delete;
    UinputDevice& operator=(const UinputDevice&) = delete;

    bool Create(const std::vector<uint16_t>& key_codes) {
        fd_.reset(open("/dev/uinput", O_WRONLY | O_NONBLOCK));
        if (!fd_.ok()) {
            PLOG(ERROR) << "Failed to open /dev/uinput";
            return false;
        }

        if (ioctl(fd_.get(), UI_SET_EVBIT, EV_KEY) < 0) {
            PLOG(ERROR) << "ioctl(UI_SET_EVBIT) failed";
            return false;
        }

        for (uint16_t key_code : key_codes) {
            if (ioctl(fd_.get(), UI_SET_KEYBIT, key_code) < 0) {
                PLOG(ERROR) << "ioctl(UI_SET_KEYBIT) failed for key_code=" << key_code;
                return false;
            }
        }

        struct uinput_setup setup = {};
        setup.id.bustype = BUS_VIRTUAL;
        setup.id.vendor = 0xCAFE;
        setup.id.product = 0x0000;
        strncpy(setup.name, "ts_vkeys", UINPUT_MAX_NAME_SIZE - 1);

        if (ioctl(fd_.get(), UI_DEV_SETUP, &setup) < 0) {
            PLOG(ERROR) << "ioctl(UI_DEV_SETUP) failed";
            return false;
        }

        if (ioctl(fd_.get(), UI_DEV_CREATE) < 0) {
            PLOG(ERROR) << "ioctl(UI_DEV_CREATE) failed";
            return false;
        }

        created_ = true;
        return true;
    }

    void SendEvent(uint16_t type, uint16_t code, int32_t value) {
        struct input_event ev = {};
        ev.type = type;
        ev.code = code;
        ev.value = value;

        if (write(fd_.get(), &ev, sizeof(ev)) < 0) {
            PLOG(ERROR) << "Failed to write input event";
        }
    }

    void ReportKey(uint16_t code, int32_t value) {
        SendEvent(EV_KEY, code, value);
        SendEvent(EV_SYN, SYN_REPORT, 0);
    }

  private:
    void Destroy() {
        if (created_ && fd_.ok()) {
            ioctl(fd_.get(), UI_DEV_DESTROY);
            created_ = false;
        }
    }

    unique_fd fd_;
    bool created_ = false;
};

class VirtualKeyMapper {
  public:
    virtual ~VirtualKeyMapper() = default;
    virtual std::optional<uint16_t> LookupKey(int32_t x, int32_t y) const = 0;
    virtual const std::vector<uint16_t>& GetKeyCodes() const = 0;
};

class ExactKeyMapper : public VirtualKeyMapper {
  public:
    void AddKey(uint16_t x, uint16_t y, uint16_t key_code) {
        key_map_[y][x] = key_code;
        key_codes_.push_back(key_code);
    }

    std::optional<uint16_t> LookupKey(int32_t x, int32_t y) const override {
        auto y_it = key_map_.find(static_cast<uint16_t>(y));
        if (y_it == key_map_.end()) {
            return std::nullopt;
        }
        auto x_it = y_it->second.find(static_cast<uint16_t>(x));
        if (x_it == y_it->second.end()) {
            return std::nullopt;
        }
        return x_it->second;
    }

    const std::vector<uint16_t>& GetKeyCodes() const override { return key_codes_; }

  private:
    std::unordered_map<uint16_t, std::unordered_map<uint16_t, uint16_t>> key_map_;
    std::vector<uint16_t> key_codes_;
};

struct VirtualKeyRegion {
    uint16_t key_code;
    int32_t x_min;
    int32_t x_max;
};

class GenVkeysMapper : public VirtualKeyMapper {
  public:
    GenVkeysMapper(uint32_t disp_maxx, uint32_t disp_maxy, uint32_t panel_maxx, uint32_t panel_maxy,
                   std::vector<uint16_t> key_codes, int32_t y_offset, bool y_beyond_maxy)
        : disp_maxy_(disp_maxy), y_beyond_maxy_(y_beyond_maxy), key_codes_(std::move(key_codes)) {
        CalculateRegions(disp_maxx, disp_maxy, panel_maxx, panel_maxy, y_offset);
    }

    std::optional<uint16_t> LookupKey(int32_t x, int32_t y) const override {
        if (y < static_cast<int32_t>(disp_maxy_)) {
            return std::nullopt;
        }
        if (!y_beyond_maxy_ && y > vkeys_maxy_) {
            return std::nullopt;
        }

        for (const auto& region : regions_) {
            if (x >= region.x_min && x <= region.x_max) {
                return region.key_code;
            }
        }
        return std::nullopt;
    }

    const std::vector<uint16_t>& GetKeyCodes() const override { return key_codes_; }

  private:
    static constexpr int kBorderAdjustNum = 3;
    static constexpr int kBorderAdjustDenom = 4;
    static constexpr int kHeightScaleNum = 8;
    static constexpr int kHeightScaleDenom = 10;

    void CalculateRegions(uint32_t disp_maxx, uint32_t disp_maxy, uint32_t panel_maxx,
                          uint32_t panel_maxy, int32_t y_offset) {
        const int num_keys = static_cast<int>(key_codes_.size());
        const int border = static_cast<int>(panel_maxx - disp_maxx) * 2;
        const int width = (static_cast<int>(disp_maxx) - (border * (num_keys - 1))) / num_keys;
        int height = static_cast<int>(panel_maxy - disp_maxy);
        height = height * kHeightScaleNum / kHeightScaleDenom;

        vkeys_maxy_ = static_cast<int32_t>(disp_maxy) + height + y_offset;

        int x2 = -border * kBorderAdjustNum / kBorderAdjustDenom;
        for (int i = 0; i < num_keys; ++i) {
            int x1 = x2 + border;
            x2 = x1 + width;

            VirtualKeyRegion region;
            region.key_code = key_codes_[i];
            region.x_min = x1;
            region.x_max = x2;
            regions_.push_back(region);

            LOG(INFO) << "Virtual key " << region.key_code << ": x=[" << region.x_min << ", "
                      << region.x_max << "], y=[" << disp_maxy << ", " << vkeys_maxy_ << "]";
        }
    }

    uint32_t disp_maxy_;
    int32_t vkeys_maxy_ = 0;
    bool y_beyond_maxy_;
    std::vector<uint16_t> key_codes_;
    std::vector<VirtualKeyRegion> regions_;
};

class TouchSlotTracker {
  public:
    explicit TouchSlotTracker(const VirtualKeyMapper& mapper) : mapper_(mapper) {}

    void SetCurrentSlot(int32_t slot) { current_slot_ = slot; }
    int32_t GetCurrentSlot() const { return current_slot_; }

    void ActivateSlot(int32_t slot) { slots_[slot].active = true; }

    void DeactivateSlot(int32_t slot) {
        auto it = slots_.find(slot);
        if (it == slots_.end() || !it->second.active) {
            return;
        }

        if (it->second.key_code != 0) {
            uinput_->ReportKey(it->second.key_code, 0);
            it->second.key_code = 0;
        }
        it->second.active = false;
    }

    void DeactivateAllSlots() {
        for (auto& [slot_num, slot] : slots_) {
            if (!slot.active) {
                continue;
            }
            if (slot.key_code != 0) {
                uinput_->ReportKey(slot.key_code, 0);
                slot.key_code = 0;
            }
            slot.active = false;
        }
    }

    void UpdateSlotX(int32_t slot, int32_t x) { slots_[slot].x = x; }
    void UpdateSlotY(int32_t slot, int32_t y) { slots_[slot].y = y; }

    void CheckAndReportKeys() {
        for (auto& [slot_num, slot] : slots_) {
            if (!slot.active) {
                continue;
            }
            auto key_code = mapper_.LookupKey(slot.x, slot.y);
            if (key_code.has_value()) {
                slot.key_code = key_code.value();
                uinput_->ReportKey(slot.key_code, 1);
            }
        }
    }

    void SetUinputDevice(UinputDevice* uinput) { uinput_ = uinput; }

  private:
    const VirtualKeyMapper& mapper_;
    UinputDevice* uinput_ = nullptr;
    std::unordered_map<int32_t, TouchSlot> slots_;
    int32_t current_slot_ = 0;
};

bool TestBit(size_t bit, const unsigned long* array) {
    constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;
    return (array[bit / kBitsPerLong] & (1UL << (bit % kBitsPerLong))) != 0;
}

std::string SanitizeDeviceName(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(c) || c == '-') {
            result += c;
        } else {
            result += '_';
        }
    }
    return result;
}

std::unique_ptr<VirtualKeyMapper> ParseExactKeyConfig(const std::string& prefix) {
    std::string names_str = GetProperty(prefix + "names", "");
    if (names_str.empty()) {
        return nullptr;
    }

    std::vector<std::string> names = Split(names_str, ",");
    if (names.empty()) {
        return nullptr;
    }

    auto mapper = std::make_unique<ExactKeyMapper>();

    for (const std::string& name : names) {
        auto x = GetUintProperty<uint16_t>(prefix + name + ".x", 0);
        auto y = GetUintProperty<uint16_t>(prefix + name + ".y", 0);
        auto key_code = GetUintProperty<uint16_t>(prefix + name + ".key_code", 0);

        if (x == 0 || y == 0 || key_code == 0) {
            LOG(ERROR) << "Virtual key '" << name << "' has missing or invalid properties";
            continue;
        }

        mapper->AddKey(x, y, key_code);
        LOG(INFO) << "Registered virtual key '" << name << "' at (" << x << ", " << y
                  << ") with key_code=" << key_code;
    }

    if (mapper->GetKeyCodes().empty()) {
        return nullptr;
    }

    return mapper;
}

std::unique_ptr<VirtualKeyMapper> ParseGenVkeysConfig(const std::string& prefix) {
    auto disp_maxx = GetUintProperty<uint32_t>(prefix + "disp_maxx", 0);
    auto disp_maxy = GetUintProperty<uint32_t>(prefix + "disp_maxy", 0);
    auto panel_maxx = GetUintProperty<uint32_t>(prefix + "panel_maxx", 0);
    auto panel_maxy = GetUintProperty<uint32_t>(prefix + "panel_maxy", 0);

    if (disp_maxx == 0 || disp_maxy == 0 || panel_maxx == 0 || panel_maxy == 0) {
        return nullptr;
    }

    std::string key_codes_str = GetProperty(prefix + "key_codes", "");
    if (key_codes_str.empty()) {
        LOG(ERROR) << "genvkeys: No key codes specified";
        return nullptr;
    }

    std::vector<std::string> key_code_strs = Split(key_codes_str, ",");
    std::vector<uint16_t> key_codes;
    key_codes.reserve(key_code_strs.size());

    for (const std::string& s : key_code_strs) {
        uint16_t value = 0;
        if (!android::base::ParseUint(s, &value) || value == 0) {
            LOG(ERROR) << "genvkeys: Invalid key code value: '" << s << "'";
            return nullptr;
        }
        key_codes.push_back(value);
    }

    if (key_codes.empty()) {
        LOG(ERROR) << "genvkeys: No valid key codes";
        return nullptr;
    }

    int32_t y_offset = android::base::GetIntProperty<int32_t>(prefix + "y_offset", 0);

    bool y_beyond_maxy = android::base::GetBoolProperty(prefix + "y_beyond_maxy", false);

    LOG(INFO) << "genvkeys: disp=" << disp_maxx << "x" << disp_maxy << " panel=" << panel_maxx
              << "x" << panel_maxy << " num_keys=" << key_codes.size() << " y_offset=" << y_offset
              << " y_beyond_maxy=" << y_beyond_maxy;

    return std::make_unique<GenVkeysMapper>(disp_maxx, disp_maxy, panel_maxx, panel_maxy,
                                            std::move(key_codes), y_offset, y_beyond_maxy);
}

std::unique_ptr<VirtualKeyMapper> ParseVirtualKeyConfig(const std::string& device_name) {
    std::vector<std::string> prefixes;

    if (!device_name.empty()) {
        std::string sanitized_name = SanitizeDeviceName(device_name);
        prefixes.push_back(std::string(kPropPrefix) + sanitized_name + ".");
    }

    prefixes.push_back(std::string(kPropPrefix));

    for (const auto& prefix : prefixes) {
        LOG(INFO) << "Trying configuration with prefix: " << prefix;

        if (auto mapper = ParseGenVkeysConfig(prefix + "genvkeys.")) {
            LOG(INFO) << "Using genvkeys configuration with prefix: " << prefix;
            return mapper;
        }

        if (auto mapper = ParseExactKeyConfig(prefix)) {
            LOG(INFO) << "Using exact key coordinate configuration with prefix: " << prefix;
            return mapper;
        }
    }

    LOG(ERROR) << "No virtual key configuration found";
    return nullptr;
}

struct TouchscreenDevice {
    unique_fd fd;
    std::string name;
};

std::optional<TouchscreenDevice> FindTouchscreenDevice() {
    for (int round = 0; round < kDeviceSearchRounds; ++round) {
        for (int node = 0; node < kDeviceSearchMaxNodes; ++node) {
            std::string path = "/dev/input/event" + std::to_string(node);
            unique_fd fd(open(path.c_str(), O_RDONLY | O_NONBLOCK));
            if (!fd.ok()) {
                continue;
            }

            constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;
            constexpr size_t kEvMaxBits = (EV_MAX + kBitsPerLong - 1) / kBitsPerLong;
            unsigned long ev_bits[kEvMaxBits] = {};

            if (ioctl(fd.get(), EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 ||
                !TestBit(EV_ABS, ev_bits)) {
                continue;
            }

            char name_buf[256] = {};
            if (ioctl(fd.get(), EVIOCGNAME(sizeof(name_buf)), name_buf) < 0) {
                name_buf[0] = '\0';
            }

            LOG(INFO) << "Found touchscreen device: " << path << " (" << name_buf << ")";

            TouchscreenDevice device;
            device.fd = std::move(fd);
            device.name = name_buf;
            return device;
        }

        if (round < kDeviceSearchRounds - 1) {
            LOG(INFO) << "Touchscreen not found, retrying in 1 second...";
            sleep(1);
        }
    }

    return std::nullopt;
}

void HandleEvent(const struct input_event& ev, TouchSlotTracker& tracker) {
    switch (ev.type) {
        case EV_ABS:
            switch (ev.code) {
                case ABS_MT_SLOT:
                    tracker.SetCurrentSlot(ev.value);
                    break;
                case ABS_MT_TRACKING_ID:
                    if (ev.value < 0) {
                        tracker.DeactivateSlot(tracker.GetCurrentSlot());
                    } else {
                        tracker.ActivateSlot(tracker.GetCurrentSlot());
                    }
                    break;
                case ABS_MT_POSITION_X:
                case ABS_X:
                    tracker.UpdateSlotX(tracker.GetCurrentSlot(), ev.value);
                    break;
                case ABS_MT_POSITION_Y:
                case ABS_Y:
                    tracker.UpdateSlotY(tracker.GetCurrentSlot(), ev.value);
                    break;
            }
            break;

        case EV_KEY:
            if (ev.code == BTN_TOUCH) {
                if (ev.value != 0) {
                    tracker.ActivateSlot(0);
                } else {
                    tracker.DeactivateAllSlots();
                }
            }
            break;

        case EV_SYN:
            if (ev.value == SYN_REPORT) {
                tracker.CheckAndReportKeys();
            }
            break;
    }
}

int RunEventLoop(unique_fd device_fd, TouchSlotTracker& tracker) {
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
                HandleEvent(ev, tracker);
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

    auto device = FindTouchscreenDevice();
    if (!device.has_value()) {
        LOG(ERROR) << "Failed to find touchscreen device";
        return EXIT_FAILURE;
    }

    auto mapper = ParseVirtualKeyConfig(device->name);
    if (!mapper) {
        return EXIT_SUCCESS;
    }

    UinputDevice uinput;
    if (!uinput.Create(mapper->GetKeyCodes())) {
        LOG(ERROR) << "Failed to create uinput device";
        return EXIT_FAILURE;
    }

    TouchSlotTracker tracker(*mapper);
    tracker.SetUinputDevice(&uinput);

    return RunEventLoop(std::move(device->fd), tracker);
}
