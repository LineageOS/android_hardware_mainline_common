/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsInput"

#include "InputDevice.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/SensorTypes.h>
#include <libsensors_common/Settings.h>
#include <libsensors_common/Sysfs.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr const char* kInputDevDir = "/dev/input";
constexpr const char* kInputSysfsDir = "/sys/class/input";
constexpr int64_t kDefaultPeriodNs = 100LL * 1000 * 1000;
constexpr int64_t kNanosecondsPerSecond = 1000LL * 1000 * 1000;
constexpr float kGravity = 9.80665f;
constexpr float kProximityFarCm = 5.0f;
constexpr float kProximityNearCm = 0.0f;
// Fallback when nothing tells us the scale of a legacy driver. Matches the
// most common legacy convention (bma150 at +/-2g, adxl34x full resolution).
constexpr float kDefaultLsbPerG = 256.0f;

struct KnownAccelerometer {
    const char* name;
    float lsb_per_g;
};

// Legacy drivers in drivers/input/misc (and friends) do not report their
// resolution; their scale is fixed by the driver source.
const KnownAccelerometer kKnownAccelerometers[] = {
        {"bma150", 256.0f},                        // 10-bit, +/-2g default
        {"mma8450", 1024.0f},                      // 12-bit, +/-2g
        {"kxtj9_accel", 1024.0f},                  // 12-bit, +/-2g default
        {"ADXL34x accelerometer", 256.0f},         // full resolution mode
        {"cma3000-accelerometer", 1000.0f},        // reports milli-g
        {"ST LIS3LV02DL Accelerometer", 1000.0f},  // reports milli-g
};

template <size_t N>
bool TestBit(const unsigned long (&bits)[N], unsigned int bit) {
    if (bit / (8 * sizeof(unsigned long)) >= N) {
        return false;
    }
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1UL;
}

constexpr size_t BitsToLongs(size_t bits) {
    return (bits + 8 * sizeof(unsigned long) - 1) / (8 * sizeof(unsigned long));
}

}  // namespace

InputDevice::InputDevice(std::string event_name, std::string dev_path, std::string sysfs_path,
                         std::string name)
    : event_name_(std::move(event_name)),
      dev_path_(std::move(dev_path)),
      sysfs_path_(std::move(sysfs_path)),
      name_(std::move(name)),
      config_key_("input." + Settings::SanitizeKeyComponent(name_)) {}

InputDevice::~InputDevice() {
    Shutdown();
}

std::unique_ptr<InputDevice> InputDevice::Probe(const std::string& event_name, int32_t* next_handle,
                                                const SensorHwdb* hwdb) {
    const std::string dev_path = std::string(kInputDevDir) + "/" + event_name;
    const std::string sysfs_path = std::string(kInputSysfsDir) + "/" + event_name + "/device";

    ::android::base::unique_fd fd(open(dev_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!fd.ok()) {
        LOG(DEBUG) << "Cannot open " << dev_path << ": " << strerror(errno);
        return nullptr;
    }

    char name_buf[256] = {};
    if (ioctl(fd.get(), EVIOCGNAME(sizeof(name_buf) - 1), name_buf) < 0) {
        LOG(DEBUG) << "EVIOCGNAME failed on " << dev_path << ": " << strerror(errno);
        return nullptr;
    }
    const std::string name = name_buf;

    unsigned long ev_bits[BitsToLongs(EV_MAX + 1)] = {};
    unsigned long abs_bits[BitsToLongs(ABS_MAX + 1)] = {};
    unsigned long sw_bits[BitsToLongs(SW_MAX + 1)] = {};
    unsigned long key_bits[BitsToLongs(KEY_MAX + 1)] = {};
    unsigned long prop_bits[BitsToLongs(INPUT_PROP_MAX + 1)] = {};
    ioctl(fd.get(), EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
    if (TestBit(ev_bits, EV_ABS)) {
        ioctl(fd.get(), EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
    }
    if (TestBit(ev_bits, EV_SW)) {
        ioctl(fd.get(), EVIOCGBIT(EV_SW, sizeof(sw_bits)), sw_bits);
    }
    if (TestBit(ev_bits, EV_KEY)) {
        ioctl(fd.get(), EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    }
    ioctl(fd.get(), EVIOCGPROP(sizeof(prop_bits)), prop_bits);

    std::unique_ptr<InputDevice> device(new InputDevice(event_name, dev_path, sysfs_path, name));
    device->modalias_ = sysfs::ReadString(sysfs_path + "/device/modalias", "");
    if (device->modalias_.empty()) {
        device->modalias_ = sysfs::ReadString(sysfs_path + "/modalias", "");
    }

    const std::string forced_type = Settings::Get().GetString(device->config_key_ + ".type", "");
    if (::android::base::EqualsIgnoreCase(forced_type, "ignore")) {
        LOG(INFO) << "Input device " << event_name << " '" << name << "' ignored by configuration";
        return nullptr;
    }

    const bool has_xyz =
            TestBit(abs_bits, ABS_X) && TestBit(abs_bits, ABS_Y) && TestBit(abs_bits, ABS_Z);
    const bool has_accel_prop = TestBit(prop_bits, INPUT_PROP_ACCELEROMETER);
    const bool looks_like_touch =
            TestBit(abs_bits, ABS_MT_POSITION_X) || TestBit(prop_bits, INPUT_PROP_DIRECT) ||
            TestBit(prop_bits, INPUT_PROP_POINTER) || TestBit(key_bits, BTN_TOUCH);
    const bool looks_like_gamepad =
            TestBit(key_bits, BTN_JOYSTICK) || TestBit(key_bits, BTN_GAMEPAD);
    const bool forced_accel = ::android::base::EqualsIgnoreCase(forced_type, "accel") ||
                              ::android::base::EqualsIgnoreCase(forced_type, "accelerometer");

    bool is_accel = false;
    if (has_xyz) {
        if (forced_accel || has_accel_prop) {
            is_accel = true;
        } else if (looks_like_touch || looks_like_gamepad) {
            LOG(DEBUG) << "Input device " << event_name << " '" << name
                       << "' has ABS_X/Y/Z but looks like a touch/game device, skipping";
        } else {
            is_accel = true;
        }
    }

    if (is_accel) {
        struct input_absinfo abs[3] = {};
        bool ok = ioctl(fd.get(), EVIOCGABS(ABS_X), &abs[0]) == 0 &&
                  ioctl(fd.get(), EVIOCGABS(ABS_Y), &abs[1]) == 0 &&
                  ioctl(fd.get(), EVIOCGABS(ABS_Z), &abs[2]) == 0;
        if (ok) {
            device->AddAccelerometer((*next_handle)++, abs, has_accel_prop, hwdb);
        } else {
            LOG(WARNING) << "EVIOCGABS failed on " << dev_path << ": " << strerror(errno);
        }
    }

    if (TestBit(sw_bits, SW_FRONT_PROXIMITY)) {
        device->AddProximitySwitch((*next_handle)++);
    }

    if (device->sensors_.empty()) {
        LOG(DEBUG) << "Input device " << event_name << " '" << name << "' provides no sensor";
        return nullptr;
    }

    LOG(INFO) << "Input device " << event_name << " '" << name << "' modalias='"
              << device->modalias_ << "' provides " << device->sensors_.size() << " sensor(s)";
    return device;
}

void InputDevice::AddAccelerometer(int32_t handle, const struct input_absinfo* abs,
                                   bool has_accel_prop, const SensorHwdb* hwdb) {
    Sensor sensor;
    sensor.kind = Kind::kAccelerometer;
    sensor.info.sensorHandle = handle;
    sensor.info.type = SensorType::ACCELEROMETER;
    ApplySensorTypeDefaults(&sensor.info);
    sensor.info.name = name_ + " Accelerometer";
    sensor.info.vendor = Settings::Get().GetString(config_key_ + ".vendor", "Linux Input");
    sensor.info.version = 1;
    sensor.period_ns = kDefaultPeriodNs;

    // Scale: configuration > evdev resolution (units per g, only meaningful
    // with INPUT_PROP_ACCELEROMETER) > known legacy driver > default.
    std::string scale_source;
    auto configured = Settings::Get().GetDouble(config_key_ + ".lsb_per_g");
    if (configured.has_value() && *configured > 0.0) {
        sensor.lsb_per_g = static_cast<float>(*configured);
        scale_source = "configuration";
    } else if (has_accel_prop && abs[0].resolution > 0) {
        sensor.lsb_per_g = static_cast<float>(abs[0].resolution);
        scale_source = "evdev resolution";
    } else {
        for (const auto& known : kKnownAccelerometers) {
            if (name_ == known.name) {
                sensor.lsb_per_g = known.lsb_per_g;
                scale_source = "known driver table";
                break;
            }
        }
        if (sensor.lsb_per_g <= 0.0f) {
            sensor.lsb_per_g = kDefaultLsbPerG;
            scale_source = "default";
            LOG(WARNING) << "Unknown scale for input accelerometer '" << name_ << "', assuming "
                         << kDefaultLsbPerG << " LSB/g. Set " << config_key_
                         << ".lsb_per_g to fix it.";
        }
    }
    sensor.info.resolution = kGravity / sensor.lsb_per_g;
    int32_t max_abs = 0;
    for (int i = 0; i < 3; i++) {
        max_abs = std::max(max_abs, std::max(std::abs(abs[i].minimum), std::abs(abs[i].maximum)));
    }
    if (max_abs > 0) {
        sensor.info.maxRange = static_cast<float>(max_abs) / sensor.lsb_per_g * kGravity;
    }

    // Mount matrix: configuration > hwdb > identity.
    std::string matrix_source = "identity";
    auto matrix_text = Settings::Get().GetString(config_key_ + ".mount_matrix");
    if (matrix_text.has_value()) {
        matrix_source = "configuration";
    } else if (hwdb != nullptr && !modalias_.empty()) {
        matrix_text = hwdb->GetMountMatrix(modalias_, "");
        if (matrix_text.has_value()) {
            matrix_source = "hwdb";
        }
    }
    if (matrix_text.has_value()) {
        auto matrix = MountMatrix::Parse(*matrix_text);
        if (matrix.has_value()) {
            sensor.mount_matrix = *matrix;
        } else {
            LOG(WARNING) << "Invalid mount matrix '" << *matrix_text << "' from " << matrix_source
                         << " for '" << name_ << "'";
            matrix_source = "identity (invalid input)";
        }
    }

    // Optional overrides.
    auto power = Settings::Get().GetDouble(config_key_ + ".power");
    if (power.has_value() && *power >= 0.0) sensor.info.power = static_cast<float>(*power);
    auto max_range = Settings::Get().GetDouble(config_key_ + ".max_range");
    if (max_range.has_value() && *max_range > 0.0) {
        sensor.info.maxRange = static_cast<float>(*max_range);
    }
    auto min_delay = Settings::Get().GetInt(config_key_ + ".min_delay_us");
    if (min_delay.has_value() && *min_delay > 0) {
        sensor.info.minDelayUs = static_cast<int32_t>(*min_delay);
    }
    auto max_delay = Settings::Get().GetInt(config_key_ + ".max_delay_us");
    if (max_delay.has_value() && *max_delay >= sensor.info.minDelayUs) {
        sensor.info.maxDelayUs = static_cast<int32_t>(*max_delay);
    }

    LOG(INFO) << "Input accelerometer '" << name_ << "': " << sensor.lsb_per_g << " LSB/g ("
              << scale_source << "), abs range [" << abs[0].minimum << ", " << abs[0].maximum
              << "], mount matrix [" << sensor.mount_matrix.ToString() << "] (" << matrix_source
              << ")";
    sensors_.emplace(handle, std::move(sensor));
}

void InputDevice::AddProximitySwitch(int32_t handle) {
    Sensor sensor;
    sensor.kind = Kind::kProximitySwitch;
    sensor.info.sensorHandle = handle;
    sensor.info.type = SensorType::PROXIMITY;
    ApplySensorTypeDefaults(&sensor.info);
    sensor.info.name = name_ + " Proximity";
    sensor.info.vendor = Settings::Get().GetString(config_key_ + ".vendor", "Linux Input");
    sensor.info.version = 1;
    sensor.info.maxRange = kProximityFarCm;
    sensor.info.resolution = kProximityFarCm;
    sensor.period_ns = kDefaultPeriodNs;
    if (!Settings::Get().GetBool(config_key_ + ".wake_up", true)) {
        sensor.info.flags &= ~SensorInfo::SENSOR_FLAG_BITS_WAKE_UP;
    }
    LOG(INFO) << "Input proximity switch '" << name_ << "' (SW_FRONT_PROXIMITY)";
    sensors_.emplace(handle, std::move(sensor));
}

std::vector<SensorInfo> InputDevice::GetSensorInfos() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SensorInfo> infos;
    for (const auto& [handle, sensor] : sensors_) {
        infos.push_back(sensor.info);
    }
    return infos;
}

bool InputDevice::HasSensor(int32_t handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sensors_.count(handle) != 0;
}

InputDevice::Sensor* InputDevice::FindSensor(int32_t handle) {
    auto it = sensors_.find(handle);
    return it == sensors_.end() ? nullptr : &it->second;
}

bool InputDevice::AnyActive() const {
    for (const auto& [handle, sensor] : sensors_) {
        if (sensor.active) {
            return true;
        }
    }
    return false;
}

void InputDevice::SetCallback(PostEventsCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void InputDevice::SetPaused(bool paused) {
    paused_.store(paused);
}

bool InputDevice::Open() {
    fd_.reset(open(dev_path_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!fd_.ok()) {
        LOG(ERROR) << "Cannot open " << dev_path_ << ": " << strerror(errno);
        return false;
    }
    int clock_id = CLOCK_BOOTTIME;
    clock_is_boottime_ = ioctl(fd_.get(), EVIOCSCLOCKID, &clock_id) == 0;
    if (!clock_is_boottime_) {
        LOG(WARNING) << "EVIOCSCLOCKID(CLOCK_BOOTTIME) failed on " << dev_path_ << ": "
                     << strerror(errno) << "; using HAL time for event timestamps";
    }

    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        LOG(ERROR) << "pipe2() failed: " << strerror(errno);
        fd_.reset();
        return false;
    }
    wake_pipe_read_.reset(pipe_fds[0]);
    wake_pipe_write_.reset(pipe_fds[1]);

    reader_running_.store(true);
    reader_thread_ = std::thread(&InputDevice::ReaderThread, this);
    LOG(INFO) << "Opened " << dev_path_ << " ('" << name_ << "')";
    return true;
}

void InputDevice::Close() {
    if (reader_thread_.joinable()) {
        reader_running_.store(false);
        char byte = 1;
        if (wake_pipe_write_.ok()) {
            TEMP_FAILURE_RETRY(write(wake_pipe_write_.get(), &byte, 1));
        }
        reader_thread_.join();
    }
    fd_.reset();
    wake_pipe_read_.reset();
    wake_pipe_write_.reset();
    LOG(INFO) << "Closed " << dev_path_ << " ('" << name_ << "')";
}

int32_t InputDevice::Activate(int32_t handle, bool enabled) {
    std::unique_lock<std::mutex> lock(mutex_);
    Sensor* sensor = FindSensor(handle);
    if (sensor == nullptr) {
        return -EINVAL;
    }
    if (sensor->active == enabled) {
        return 0;
    }

    if (enabled) {
        const bool was_open = fd_.ok();
        if (!was_open && !Open()) {
            return -EIO;
        }
        sensor->active = true;
        sensor->last_event.reset();
        sensor->last_emit_ns = 0;
        lock.unlock();
        EmitInitialState();
        return 0;
    }

    sensor->active = false;
    sensor->last_event.reset();
    if (!AnyActive()) {
        lock.unlock();
        Close();
    }
    return 0;
}

int32_t InputDevice::Batch(int32_t handle, int64_t period_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    Sensor* sensor = FindSensor(handle);
    if (sensor == nullptr) {
        return -EINVAL;
    }
    sensor->period_ns = period_ns > 0 ? period_ns : kDefaultPeriodNs;
    return 0;
}

void InputDevice::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [handle, sensor] : sensors_) {
            sensor.active = false;
        }
    }
    Close();
}

void InputDevice::PostEvent(const Event& event, bool wakeup) {
    PostEventsCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
    }
    if (callback && !paused_.load()) {
        LOG(VERBOSE) << "Input event: " << EventToString(event);
        callback({event}, wakeup);
    }
}

void InputDevice::EmitInitialState() {
    // Report the current state right away: on-change sensors must deliver an
    // event upon activation and polled accelerometers may take a while before
    // their first report.
    std::vector<std::pair<Event, bool>> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!fd_.ok()) {
            return;
        }
        for (auto& [handle, sensor] : sensors_) {
            if (!sensor.active) {
                continue;
            }
            if (sensor.kind == Kind::kAccelerometer) {
                struct input_absinfo abs[3] = {};
                if (ioctl(fd_.get(), EVIOCGABS(ABS_X), &abs[0]) != 0 ||
                    ioctl(fd_.get(), EVIOCGABS(ABS_Y), &abs[1]) != 0 ||
                    ioctl(fd_.get(), EVIOCGABS(ABS_Z), &abs[2]) != 0) {
                    continue;
                }
                for (int i = 0; i < 3; i++) {
                    sensor.raw[i] = abs[i].value;
                }
                sensor.raw_dirty = true;
            } else if (sensor.kind == Kind::kProximitySwitch) {
                unsigned long sw_bits[BitsToLongs(SW_MAX + 1)] = {};
                if (ioctl(fd_.get(), EVIOCGSW(sizeof(sw_bits)), sw_bits) != 0) {
                    continue;
                }
                sensor.raw[0] = TestBit(sw_bits, SW_FRONT_PROXIMITY) ? 1 : 0;
                sensor.raw_dirty = true;
            }
        }
    }
    HandleSyncReport(GetBootTimeNs());
}

void InputDevice::HandleSwitchEvent(int code, int value, int64_t /* timestamp_ns */) {
    if (code != SW_FRONT_PROXIMITY) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, sensor] : sensors_) {
        if (sensor.kind == Kind::kProximitySwitch) {
            sensor.raw[0] = value ? 1 : 0;
            sensor.raw_dirty = true;
        }
    }
}

void InputDevice::HandleSyncReport(int64_t timestamp_ns) {
    std::vector<std::pair<Event, bool>> to_post;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [handle, sensor] : sensors_) {
            if (!sensor.active || !sensor.raw_dirty) {
                continue;
            }
            sensor.raw_dirty = false;

            Event event;
            if (sensor.kind == Kind::kAccelerometer) {
                // Decimate to the requested rate; the kernel driver polls at
                // its own fixed rate.
                if (sensor.last_emit_ns != 0 &&
                    timestamp_ns - sensor.last_emit_ns < sensor.period_ns - sensor.period_ns / 10) {
                    continue;
                }
                float x = static_cast<float>(sensor.raw[0]) / sensor.lsb_per_g * kGravity;
                float y = static_cast<float>(sensor.raw[1]) / sensor.lsb_per_g * kGravity;
                float z = static_cast<float>(sensor.raw[2]) / sensor.lsb_per_g * kGravity;
                sensor.mount_matrix.Apply(&x, &y, &z);
                event = MakeVec3Event(handle, sensor.info.type, timestamp_ns, x, y, z);
            } else {
                const float distance = sensor.raw[0] ? kProximityNearCm : kProximityFarCm;
                event = MakeScalarEvent(handle, sensor.info.type, timestamp_ns, distance);
                if (sensor.last_event.has_value() && HaveSamePayload(*sensor.last_event, event)) {
                    continue;
                }
            }
            sensor.last_event = event;
            sensor.last_emit_ns = timestamp_ns;
            to_post.emplace_back(event, IsWakeUpSensor(sensor.info.flags));
        }
    }
    for (const auto& [event, wakeup] : to_post) {
        PostEvent(event, wakeup);
    }
}

void InputDevice::ReaderThread() {
    pthread_setname_np(pthread_self(), ("input-" + event_name_).substr(0, 15).c_str());
    LOG(DEBUG) << "Reader thread started for " << dev_path_;

    struct pollfd fds[2];
    fds[0].fd = fd_.get();
    fds[0].events = POLLIN;
    fds[1].fd = wake_pipe_read_.get();
    fds[1].events = POLLIN;

    struct input_event events[64];
    while (reader_running_.load()) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        int ret = poll(fds, 2, 1000);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "poll() failed on " << dev_path_ << ": " << strerror(errno);
            break;
        }
        if (ret == 0 || (fds[1].revents & POLLIN)) {
            continue;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG(WARNING) << dev_path_ << " went away";
            break;
        }
        if (!(fds[0].revents & POLLIN)) {
            continue;
        }

        ssize_t bytes = TEMP_FAILURE_RETRY(read(fd_.get(), events, sizeof(events)));
        if (bytes < 0) {
            if (errno == EAGAIN) {
                continue;
            }
            LOG(ERROR) << "read() failed on " << dev_path_ << ": " << strerror(errno);
            break;
        }
        const size_t count = static_cast<size_t>(bytes) / sizeof(struct input_event);
        for (size_t i = 0; i < count; i++) {
            const struct input_event& ev = events[i];
            int64_t timestamp_ns =
                    clock_is_boottime_
                            ? static_cast<int64_t>(ev.input_event_sec) * kNanosecondsPerSecond +
                                      static_cast<int64_t>(ev.input_event_usec) * 1000
                            : GetBootTimeNs();
            switch (ev.type) {
                case EV_ABS: {
                    int axis = -1;
                    if (ev.code == ABS_X) axis = 0;
                    if (ev.code == ABS_Y) axis = 1;
                    if (ev.code == ABS_Z) axis = 2;
                    if (axis < 0) {
                        break;
                    }
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (auto& [handle, sensor] : sensors_) {
                        if (sensor.kind == Kind::kAccelerometer) {
                            sensor.raw[axis] = ev.value;
                            sensor.raw_dirty = true;
                        }
                    }
                    break;
                }
                case EV_SW:
                    HandleSwitchEvent(ev.code, ev.value, timestamp_ns);
                    break;
                case EV_SYN:
                    if (ev.code == SYN_REPORT) {
                        HandleSyncReport(timestamp_ns);
                    }
                    break;
                default:
                    break;
            }
        }
    }
    LOG(DEBUG) << "Reader thread stopped for " << dev_path_;
}

}  // namespace aidl::android::hardware::sensors::mainline
