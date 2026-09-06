/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsIio"

#include "IioDevice.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/SensorTypes.h>
#include <libsensors_common/Settings.h>
#include <libsensors_common/Sysfs.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr int kBufferLength = 128;
constexpr int64_t kNanosecondsPerSecond = 1000LL * 1000 * 1000;
// A buffer timestamp further away than this from now is not trusted.
constexpr int64_t kTimestampSanityNs = 2 * kNanosecondsPerSecond;
// Minimum time without data before the buffer is considered dead.
constexpr int64_t kMinWatchdogNs = 3 * kNanosecondsPerSecond;
constexpr int kPollTimeoutMs = 500;

std::string FirstCompatible(const std::string& content) {
    // of_node/compatible holds NUL separated strings; ReadString only strips
    // trailing NULs.
    size_t nul = content.find('\0');
    return nul == std::string::npos ? content : content.substr(0, nul);
}

}  // namespace

IioDevice::IioDevice(IioDeviceInfo info) : info_(std::move(info)) {
    dev_node_ = "/dev/iio:device" + std::to_string(info_.dev_num);
}

IioDevice::~IioDevice() {
    Shutdown();
}

std::unique_ptr<IioDevice> IioDevice::Discover(int dev_num, const std::string& sysfs_path,
                                               const SensorHwdb* hwdb, int32_t* next_handle) {
    IioDeviceInfo info;
    info.dev_num = dev_num;
    info.sysfs_path = sysfs::RealPath(sysfs_path);
    info.name = sysfs::ReadString(sysfs_path + "/name", "");
    info.label = sysfs::ReadString(sysfs_path + "/label", "");
    info.of_name = sysfs::ReadString(sysfs_path + "/of_node/name", "");
    info.compatible = FirstCompatible(sysfs::ReadString(sysfs_path + "/of_node/compatible", ""));
    info.modalias = sysfs::ReadString(info.sysfs_path + "/../modalias", "");
    if (info.modalias.empty()) {
        info.modalias = sysfs::ReadString(info.sysfs_path + "/../../modalias", "");
    }
    info.quirks = GetIioDeviceQuirks(info.name);

    // Human readable model: the driver name unless it is just a bus address
    // (e.g. ak8975 probed from device tree reports "3-000c").
    if (info.name.empty() || LooksLikeBusAddress(info.name)) {
        std::string model = ModelFromCompatible(info.compatible);
        if (model.empty()) model = info.of_name;
        if (model.empty()) model = info.name;
        if (model.empty()) model = "iio:device" + std::to_string(dev_num);
        info.model = model;
    } else {
        info.model = info.name;
    }
    info.config_key = "iio." + Settings::SanitizeKeyComponent(info.model);
    if (info.model != info.name && !info.name.empty()) {
        info.alt_config_key = "iio." + Settings::SanitizeKeyComponent(info.name);
    }

    LOG(INFO) << "IIO device " << dev_num << ": name='" << info.name << "' label='" << info.label
              << "' of_name='" << info.of_name << "' compatible='" << info.compatible
              << "' modalias='" << info.modalias << "' path=" << info.sysfs_path;

    if (Settings::Get().GetFirstBool(info.ConfigKeys("disable")).value_or(false)) {
        LOG(INFO) << "IIO device " << dev_num << " ('" << info.model
                  << "') disabled by configuration";
        return nullptr;
    }

    std::unique_ptr<IioDevice> device(new IioDevice(std::move(info)));
    device->ProbeChannels();
    device->CreateSensors(hwdb, next_handle);
    if (device->sensors_.empty()) {
        auto guess = GuessSensorTypeFromName(device->info_.name + " " + device->info_.of_name +
                                             " " + device->info_.compatible);
        if (guess.has_value()) {
            LOG(WARNING) << "IIO device " << dev_num << " ('" << device->info_.model
                         << "') looks like a " << toString(*guess)
                         << " but exposes no supported channels, ignoring";
        } else {
            LOG(INFO) << "IIO device " << dev_num << " ('" << device->info_.model
                      << "') provides no supported sensor";
        }
        return nullptr;
    }
    return device;
}

IioChannel* IioDevice::GetOrCreateChannel(const IioChannelId& id) {
    const std::string key = id.Key();
    auto it = channels_.find(key);
    if (it == channels_.end()) {
        auto channel = std::make_unique<IioChannel>();
        channel->id = id;
        it = channels_.emplace(key, std::move(channel)).first;
    }
    return it->second.get();
}

void IioDevice::ProbeChannels() {
    // Shared-by-type scale/offset attributes ("in_accel_scale").
    std::map<std::string, double> type_scale;
    std::map<std::string, double> type_offset;

    for (const auto& entry : sysfs::ListDirectory(info_.sysfs_path)) {
        auto parsed = ParseIioAttributeName(entry);
        if (!parsed.has_value()) {
            continue;
        }
        const std::string path = info_.sysfs_path + "/" + entry;
        const bool per_channel = !parsed->id.modifier.empty() || parsed->id.index >= 0;
        if (parsed->postfix == "raw" || parsed->postfix == "input") {
            IioChannel* channel = GetOrCreateChannel(parsed->id);
            const bool processed = parsed->postfix == "input";
            // Prefer processed values over raw ones.
            if (channel->poll_attribute.empty() || (processed && !channel->poll_is_processed)) {
                channel->poll_attribute = entry;
                channel->poll_is_processed = processed;
            }
        } else if (parsed->postfix == "scale" || parsed->postfix == "offset") {
            auto value = sysfs::ReadDouble(path);
            if (!value.has_value()) {
                LOG(WARNING) << "Unreadable attribute " << path;
                continue;
            }
            if (per_channel) {
                IioChannel* channel = GetOrCreateChannel(parsed->id);
                if (parsed->postfix == "scale") {
                    channel->scale = *value;
                    channel->has_scale = true;
                } else {
                    channel->offset = *value;
                    channel->has_offset = true;
                }
            } else if (parsed->postfix == "scale") {
                type_scale[parsed->id.type] = *value;
            } else {
                type_offset[parsed->id.type] = *value;
            }
        }
    }

    // Scan elements.
    if (sysfs::IsDirectory(info_.sysfs_path + "/scan_elements")) {
        scan_dir_ = "scan_elements";
    } else if (sysfs::IsDirectory(info_.sysfs_path + "/buffer0")) {
        scan_dir_ = "buffer0";
    }
    if (!scan_dir_.empty()) {
        const std::string dir = info_.sysfs_path + "/" + scan_dir_;
        for (const auto& entry : sysfs::ListDirectory(dir)) {
            auto parsed = ParseIioAttributeName(entry);
            if (!parsed.has_value() || parsed->postfix != "en") {
                continue;
            }
            IioChannel* channel = GetOrCreateChannel(parsed->id);
            const std::string key = parsed->id.Key();
            auto type_text = sysfs::ReadString(dir + "/" + key + "_type");
            auto index = sysfs::ReadInt(dir + "/" + key + "_index");
            if (!type_text.has_value() || !index.has_value() || *index < 0) {
                LOG(DEBUG) << "Scan element " << key << " of device " << info_.dev_num
                           << " has no usable type/index";
                continue;
            }
            auto scan_type = IioScanType::Parse(*type_text);
            if (!scan_type.has_value()) {
                LOG(WARNING) << "Unparsable scan type '" << *type_text << "' for " << key
                             << " of device " << info_.dev_num;
                continue;
            }
            channel->has_scan_element = true;
            channel->scan_index = static_cast<int>(*index);
            channel->scan_type = *scan_type;
            if (channel->IsTimestamp()) {
                timestamp_channel_ = channel;
            }
        }
    }

    // Resolve shared attributes and log the channel table.
    for (auto& [key, channel] : channels_) {
        if (!channel->has_scale) {
            auto it = type_scale.find(channel->id.type);
            if (it != type_scale.end()) {
                channel->scale = it->second;
                channel->has_scale = true;
            }
        }
        if (!channel->has_offset) {
            auto it = type_offset.find(channel->id.type);
            if (it != type_offset.end()) {
                channel->offset = it->second;
                channel->has_offset = true;
            }
        }
        LOG(INFO) << "  channel " << key << ": poll='" << channel->poll_attribute
                  << (channel->poll_is_processed ? "' (processed)" : "'")
                  << " scan=" << (channel->has_scan_element ? channel->scan_type.ToString() : "-")
                  << " index=" << channel->scan_index << " scale=" << channel->scale
                  << (channel->has_scale ? "" : " (default)") << " offset=" << channel->offset;
    }

    const bool has_dev_node = sysfs::Exists(dev_node_);
    const bool has_buffer_dir = sysfs::Exists(info_.sysfs_path + "/buffer/enable");
    const bool has_scan = std::any_of(channels_.begin(), channels_.end(), [](const auto& entry) {
        return entry.second->has_scan_element && !entry.second->IsTimestamp();
    });
    std::string mode = Settings::Get().GetFirstString(info_.ConfigKeys("mode")).value_or("auto");
    info_.buffer_capable = has_dev_node && has_buffer_dir && has_scan &&
                           !::android::base::EqualsIgnoreCase(mode, "poll");
    if (::android::base::EqualsIgnoreCase(mode, "buffer") && !info_.buffer_capable) {
        LOG(WARNING) << "IIO device " << info_.dev_num
                     << ": buffer mode requested but not available (dev node=" << has_dev_node
                     << " buffer=" << has_buffer_dir << " scan elements=" << has_scan << ")";
    }
    LOG(INFO) << "IIO device " << info_.dev_num << ": " << channels_.size()
              << " channel(s), dev node=" << has_dev_node << " buffer=" << has_buffer_dir
              << " scan_dir=" << scan_dir_ << " mode setting=" << mode
              << " -> buffer capable=" << info_.buffer_capable;
}

void IioDevice::CreateSensors(const SensorHwdb* hwdb, int32_t* next_handle) {
    std::vector<IioChannelId> readable;
    for (const auto& [key, channel] : channels_) {
        if (!channel->poll_attribute.empty() || channel->has_scan_element) {
            readable.push_back(channel->id);
        }
    }
    const bool expose_temperature =
            Settings::Get().GetFirstBool(info_.ConfigKeys("expose_temperature")).value_or(false);
    std::vector<IioSensorSpec> specs =
            MatchIioSensorSpecs(readable, info_.quirks, expose_temperature);

    // Collect channels per spec first so that the buffer policy can be decided
    // for the whole device: mixing buffer and sysfs reads is not supported by
    // every driver (read_raw returns EBUSY while the buffer is enabled), so
    // buffer mode is only used if every sensor of the device can use it.
    std::vector<std::vector<IioChannel*>> spec_channels;
    bool all_bufferable = true;
    for (const auto& spec : specs) {
        std::vector<IioChannel*> list;
        for (const auto& modifier : spec.modifiers) {
            IioChannelId id;
            id.type = spec.iio_type;
            id.index = spec.index;
            id.modifier = modifier;
            auto it = channels_.find(id.Key());
            if (it != channels_.end()) {
                list.push_back(it->second.get());
            }
        }
        for (const IioChannel* channel : list) {
            if (!channel->has_scan_element) {
                all_bufferable = false;
            }
        }
        spec_channels.push_back(std::move(list));
    }
    if (info_.buffer_capable && !all_bufferable) {
        LOG(INFO) << "IIO device " << info_.dev_num
                  << ": not every sensor can use the buffer, using poll mode for the device";
        info_.buffer_capable = false;
    }

    for (size_t i = 0; i < specs.size(); i++) {
        if (spec_channels[i].size() != specs[i].modifiers.size()) {
            continue;
        }
        auto sensor =
                std::make_unique<IioSensor>(info_, *next_handle, specs[i], spec_channels[i], hwdb);
        if (!sensor->CanBuffer() && !sensor->CanPoll()) {
            LOG(WARNING) << "IIO sensor " << sensor->GetInfo().name
                         << " can neither be buffered nor polled, skipping";
            continue;
        }
        (*next_handle)++;
        LOG(INFO) << "IIO sensor discovered: " << sensor->Describe();
        sensors_.push_back(std::move(sensor));
    }
}

std::vector<IioSensor*> IioDevice::GetSensors() {
    std::vector<IioSensor*> list;
    for (auto& sensor : sensors_) {
        list.push_back(sensor.get());
    }
    return list;
}

IioSensor* IioDevice::FindSensor(int32_t handle) {
    for (auto& sensor : sensors_) {
        if (sensor->GetHandle() == handle) {
            return sensor.get();
        }
    }
    return nullptr;
}

void IioDevice::SetCallback(PostEventsCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void IioDevice::SetPaused(bool paused) {
    paused_.store(paused);
}

void IioDevice::PostEvents(const std::vector<Event>& events, bool wakeup) {
    if (events.empty() || paused_.load()) {
        return;
    }
    PostEventsCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = callback_;
    }
    if (callback) {
        for (const Event& event : events) {
            LOG(VERBOSE) << "IIO event: " << EventToString(event);
        }
        callback(events, wakeup);
    }
}

bool IioDevice::AnyActiveLocked() const {
    return std::any_of(sensors_.begin(), sensors_.end(),
                       [](const auto& sensor) { return sensor->IsActive(); });
}

int32_t IioDevice::Activate(int32_t handle, bool enabled) {
    std::unique_lock<std::mutex> lock(mutex_);
    IioSensor* sensor = FindSensor(handle);
    if (sensor == nullptr) {
        return -EINVAL;
    }
    if (sensor->IsActive() == enabled) {
        return 0;
    }

    if (enabled) {
        sensor->ResetFilterState();
        sensor->SetActive(true);

        if (mode_ == Mode::kIdle) {
            if (info_.buffer_capable && !buffer_failed_) {
                if (StartBufferLocked()) {
                    mode_ = Mode::kBuffer;
                } else {
                    LOG(WARNING) << "IIO device " << info_.dev_num
                                 << ": buffer mode failed, falling back to poll mode";
                    buffer_failed_ = true;
                }
            }
            if (mode_ == Mode::kIdle) {
                mode_ = Mode::kPoll;
            }
        }

        if (mode_ == Mode::kBuffer) {
            ApplyRatesLocked();
        } else if (sensor->CanPoll()) {
            StartPollLocked(sensor);
        } else {
            LOG(ERROR) << "IIO sensor " << sensor->GetInfo().name
                       << " cannot be polled and the device buffer is unavailable";
            sensor->SetActive(false);
            if (!AnyActiveLocked()) {
                mode_ = Mode::kIdle;
            }
            return -EIO;
        }
        LOG(INFO) << "IIO sensor " << handle << " ('" << sensor->GetInfo().name
                  << "') activated in " << (mode_ == Mode::kBuffer ? "buffer" : "poll") << " mode";
        return 0;
    }

    sensor->SetActive(false);
    if (mode_ == Mode::kPoll) {
        StopPoll(&lock, sensor);
    }
    if (!AnyActiveLocked()) {
        if (mode_ == Mode::kBuffer) {
            StopBuffer(&lock);
        }
        mode_ = Mode::kIdle;
    } else if (mode_ == Mode::kBuffer) {
        ApplyRatesLocked();
    }
    LOG(INFO) << "IIO sensor " << handle << " ('" << sensor->GetInfo().name << "') deactivated";
    return 0;
}

int32_t IioDevice::SetPeriod(int32_t handle, int64_t period_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    IioSensor* sensor = FindSensor(handle);
    if (sensor == nullptr) {
        return -EINVAL;
    }
    sensor->SetPeriodNs(period_ns);
    LOG(DEBUG) << "IIO sensor " << handle << " period " << period_ns / 1000 << " us";
    if (!sensor->IsActive()) {
        return 0;
    }
    if (mode_ == Mode::kBuffer) {
        ApplyRatesLocked();
    } else if (mode_ == Mode::kPoll && sensor->GetPollWorker() != nullptr) {
        sensor->GetPollWorker()->SetPeriod(sensor->GetPollPeriodNs());
    }
    return 0;
}

void IioDevice::Shutdown() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (auto& sensor : sensors_) {
        sensor->SetActive(false);
        if (mode_ == Mode::kPoll) {
            StopPoll(&lock, sensor.get());
        }
    }
    if (mode_ == Mode::kBuffer) {
        StopBuffer(&lock);
    }
    if (reader_thread_.joinable()) {
        lock.unlock();
        reader_thread_.join();
        lock.lock();
    }
    mode_ = Mode::kIdle;
}

// ---------------------------------------------------------------------------
// Poll mode

void IioDevice::StartPollLocked(IioSensor* sensor) {
    if (sensor->GetPollWorker() == nullptr) {
        sensor->SetPollWorker(std::make_unique<PeriodicWorker>(
                "iio" + std::to_string(info_.dev_num) + "-" + std::to_string(sensor->GetHandle()),
                [this, sensor]() { PollSensor(sensor); }));
    }
    sensor->GetPollWorker()->Start(sensor->GetPollPeriodNs());
}

void IioDevice::StopPoll(std::unique_lock<std::mutex>* lock, IioSensor* sensor) {
    PeriodicWorker* worker = sensor->GetPollWorker();
    if (worker == nullptr || !worker->IsRunning()) {
        return;
    }
    // The worker never takes mutex_ but may be blocked in the frontend
    // callback; do not hold our lock while joining it.
    lock->unlock();
    worker->Stop();
    lock->lock();
}

void IioDevice::PollSensor(IioSensor* sensor) {
    if (!sensor->IsActive()) {
        return;
    }
    auto event = sensor->ReadEventFromSysfs(GetBootTimeNs());
    if (!event.has_value()) {
        return;
    }
    auto filtered = sensor->Filter(*event);
    if (filtered.has_value()) {
        PostEvents({*filtered}, IsWakeUpSensor(sensor->GetInfo().flags));
    }
}

// ---------------------------------------------------------------------------
// Sampling rates

double IioDevice::RoundFrequency(const std::string& available_attr, double hz) {
    auto content = sysfs::ReadString(available_attr);
    if (!content.has_value()) {
        return hz;
    }
    std::vector<double> available = sysfs::ParseDoubleList(*content);
    if (available.empty()) {
        return hz;
    }
    if (content->find('[') != std::string::npos && available.size() == 2) {
        // "[min step max]" range: clamp.
        return std::clamp(hz, available.front(), available.back());
    }
    // Smallest available frequency that is not slower than requested.
    for (double candidate : available) {
        if (candidate + 1e-6 >= hz) {
            return candidate;
        }
    }
    return available.back();
}

void IioDevice::WriteSamplingFrequency(const std::string& iio_type, double hz) {
    struct Candidate {
        std::string attr;
        std::string available;
    };
    const std::vector<Candidate> candidates = {
            {"in_" + iio_type + "_sampling_frequency",
             "in_" + iio_type + "_sampling_frequency_available"},
            {"sampling_frequency", "sampling_frequency_available"},
            {"buffer/sampling_frequency", "buffer/sampling_frequency_available"},
    };
    for (const auto& candidate : candidates) {
        const std::string path = info_.sysfs_path + "/" + candidate.attr;
        if (!sysfs::Exists(path)) {
            continue;
        }
        double value = RoundFrequency(info_.sysfs_path + "/" + candidate.available, hz);
        auto last = written_frequencies_.find(path);
        if (last != written_frequencies_.end() && std::fabs(last->second - value) < 1e-6) {
            continue;
        }
        // Drivers with integer-only rates reject fractional strings.
        bool ok = (std::fabs(value - std::round(value)) < 1e-6)
                          ? sysfs::WriteInt(path, static_cast<int64_t>(std::round(value)))
                          : sysfs::WriteDouble(path, value);
        if (ok) {
            written_frequencies_[path] = value;
            LOG(INFO) << "IIO device " << info_.dev_num << ": " << candidate.attr << " = " << value
                      << " Hz (requested " << hz << ")";
        } else {
            // Typically -EBUSY while the buffer is enabled; the previously
            // programmed rate stays in effect.
            LOG(WARNING) << "IIO device " << info_.dev_num << ": failed to write " << value
                         << " Hz to " << candidate.attr << ": " << strerror(errno);
        }
        // Only the most specific attribute is written, except that hardware
        // FIFO devices also expose buffer/sampling_frequency.
        if (candidate.attr != "buffer/sampling_frequency") {
            const std::string fifo_path = info_.sysfs_path + "/buffer/sampling_frequency";
            if (sysfs::Exists(fifo_path)) {
                sysfs::WriteInt(fifo_path, static_cast<int64_t>(std::max(1.0, std::round(value))));
            }
        }
        return;
    }
    LOG(DEBUG) << "IIO device " << info_.dev_num << ": no sampling frequency attribute for "
               << iio_type;
}

void IioDevice::ApplyRatesLocked() {
    double device_hz = 0.0;
    std::vector<std::string> types;
    for (const auto& sensor : sensors_) {
        if (!sensor->IsActive()) {
            continue;
        }
        device_hz = std::max(device_hz, sensor->GetRequestedFrequencyHz());
        const std::string& type = sensor->GetSpec().iio_type;
        if (std::find(types.begin(), types.end(), type) == types.end()) {
            types.push_back(type);
        }
    }
    if (device_hz <= 0.0) {
        return;
    }
    for (const auto& type : types) {
        WriteSamplingFrequency(type, device_hz);
    }
    if (trigger_ && trigger_->SupportsFrequency()) {
        trigger_->SetFrequency(device_hz);
    }
}

// ---------------------------------------------------------------------------
// Buffer mode

bool IioDevice::EnableScanElementsLocked() {
    const std::string dir = info_.sysfs_path + "/" + scan_dir_;
    for (auto& [key, channel] : channels_) {
        if (!channel->has_scan_element) {
            continue;
        }
        const std::string path = dir + "/" + key + "_en";
        if (!sysfs::WriteInt(path, 1)) {
            LOG(DEBUG) << "Cannot enable scan element " << key << ": " << strerror(errno);
        }
    }
    scan_channels_.clear();
    for (auto& [key, channel] : channels_) {
        if (!channel->has_scan_element) {
            continue;
        }
        channel->enabled_in_scan = sysfs::ReadInt(dir + "/" + key + "_en", 0) == 1;
        if (channel->enabled_in_scan) {
            scan_channels_.push_back(channel.get());
        }
    }
    scan_size_ = ComputeScanLayout(&scan_channels_);
    std::string layout;
    for (const IioChannel* channel : scan_channels_) {
        layout += " " + channel->id.Key() + "@" + std::to_string(channel->location);
    }
    LOG(INFO) << "IIO device " << info_.dev_num << ": scan size " << scan_size_
              << " bytes:" << layout;
    return scan_size_ > 0;
}

bool IioDevice::StartBufferLocked() {
    const std::string enable_path = info_.sysfs_path + "/buffer/enable";
    written_frequencies_.clear();

    // Timestamps in the Android time base.
    const std::string clock_path = info_.sysfs_path + "/current_timestamp_clock";
    timestamp_is_boottime_ = false;
    if (sysfs::Exists(clock_path)) {
        sysfs::WriteString(clock_path, "boottime");
        timestamp_is_boottime_ = sysfs::ReadString(clock_path, "") == "boottime";
        if (!timestamp_is_boottime_) {
            LOG(WARNING) << "IIO device " << info_.dev_num
                         << ": could not select the boottime clock, using HAL timestamps";
        }
    }

    // Make sure we start from a disabled buffer (e.g. after a crash).
    sysfs::WriteInt(enable_path, 0);

    if (IioTrigger::DeviceHasTriggerInterface(info_.sysfs_path)) {
        trigger_ = IioTrigger::Assign(info_.sysfs_path, info_.dev_num, info_.name);
        if (!trigger_) {
            LOG(INFO) << "IIO device " << info_.dev_num
                      << ": no trigger, trying to enable the buffer anyway";
        }
    }

    if (!EnableScanElementsLocked()) {
        LOG(WARNING) << "IIO device " << info_.dev_num << ": no scan element could be enabled";
        trigger_.reset();
        return false;
    }

    sysfs::WriteInt(info_.sysfs_path + "/buffer/length", kBufferLength);
    sysfs::WriteInt(info_.sysfs_path + "/buffer/watermark", 1);
    ApplyRatesLocked();

    if (!sysfs::WriteInt(enable_path, 1) || sysfs::ReadInt(enable_path, 0) != 1) {
        LOG(WARNING) << "IIO device " << info_.dev_num
                     << ": enabling the buffer failed: " << strerror(errno);
        sysfs::WriteInt(enable_path, 0);
        trigger_.reset();
        return false;
    }

    fd_.reset(open(dev_node_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
    if (!fd_.ok()) {
        LOG(WARNING) << "Cannot open " << dev_node_ << ": " << strerror(errno);
        sysfs::WriteInt(enable_path, 0);
        trigger_.reset();
        return false;
    }
    int pipe_fds[2] = {-1, -1};
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        LOG(ERROR) << "pipe2() failed: " << strerror(errno);
        fd_.reset();
        sysfs::WriteInt(enable_path, 0);
        trigger_.reset();
        return false;
    }
    wake_pipe_read_.reset(pipe_fds[0]);
    wake_pipe_write_.reset(pipe_fds[1]);

    if (reader_thread_.joinable()) {
        // Previous reader already exited (fallback path); reap it.
        reader_thread_.join();
    }
    reader_stop_.store(false);
    reader_thread_ = std::thread(&IioDevice::ReaderThread, this);
    LOG(INFO) << "IIO device " << info_.dev_num << ": buffer enabled"
              << (trigger_ ? " with trigger '" + trigger_->GetName() + "'" : " without trigger");
    return true;
}

void IioDevice::ReleaseBufferResourcesLocked() {
    sysfs::WriteInt(info_.sysfs_path + "/buffer/enable", 0);
    fd_.reset();
    wake_pipe_read_.reset();
    wake_pipe_write_.reset();
    trigger_.reset();
    for (IioChannel* channel : scan_channels_) {
        channel->enabled_in_scan = false;
    }
    scan_channels_.clear();
    scan_size_ = 0;
    LOG(INFO) << "IIO device " << info_.dev_num << ": buffer disabled";
}

void IioDevice::StopBuffer(std::unique_lock<std::mutex>* lock) {
    reader_stop_.store(true);
    if (wake_pipe_write_.ok()) {
        char byte = 1;
        TEMP_FAILURE_RETRY(write(wake_pipe_write_.get(), &byte, 1));
    }
    if (reader_thread_.joinable()) {
        lock->unlock();
        reader_thread_.join();
        lock->lock();
    }
    // The reader may have switched the device to poll mode in the meantime, in
    // which case the resources are already released.
    if (mode_ == Mode::kBuffer) {
        ReleaseBufferResourcesLocked();
    }
}

int64_t IioDevice::WatchdogTimeoutNsLocked() const {
    int64_t max_period = 0;
    for (const auto& sensor : sensors_) {
        if (sensor->IsActive()) {
            max_period = std::max(max_period, sensor->GetPeriodNs());
        }
    }
    return std::max(kMinWatchdogNs, 5 * max_period);
}

int64_t IioDevice::TimestampFromScan(const uint8_t* scan, int64_t now_ns) {
    if (timestamp_channel_ == nullptr || !timestamp_channel_->enabled_in_scan ||
        !timestamp_is_boottime_ || info_.quirks.ignore_timestamp_channel ||
        timestamp_channel_->scan_type.realbits < 64) {
        return now_ns;
    }
    const int64_t ts = timestamp_channel_->DecodeRaw(scan);
    if (ts <= 0 || std::llabs(ts - now_ns) > kTimestampSanityNs) {
        if (!timestamp_warned_) {
            timestamp_warned_ = true;
            LOG(WARNING) << "IIO device " << info_.dev_num << ": implausible buffer timestamp "
                         << ts << " (now " << now_ns << "), using HAL timestamps";
        }
        return now_ns;
    }
    return ts;
}

void IioDevice::HandleScans(const uint8_t* data, size_t count) {
    std::vector<Event> events;
    bool wakeup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mode_ != Mode::kBuffer) {
            return;
        }
        const int64_t now = GetBootTimeNs();
        for (size_t i = 0; i < count; i++) {
            const uint8_t* scan = data + i * scan_size_;
            const int64_t timestamp = TimestampFromScan(scan, now);
            for (auto& sensor : sensors_) {
                if (!sensor->IsActive()) {
                    continue;
                }
                auto event = sensor->BuildEventFromScan(scan, timestamp);
                if (!event.has_value()) {
                    continue;
                }
                auto filtered = sensor->Filter(*event);
                if (filtered.has_value()) {
                    events.push_back(*filtered);
                    wakeup = wakeup || IsWakeUpSensor(sensor->GetInfo().flags);
                }
            }
        }
    }
    // Never call into the frontend with our lock held.
    PostEvents(events, wakeup);
}

void IioDevice::FallbackToPollFromReader() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != Mode::kBuffer) {
        return;
    }
    ReleaseBufferResourcesLocked();
    buffer_failed_ = true;
    bool all_pollable = true;
    for (auto& sensor : sensors_) {
        if (sensor->IsActive() && !sensor->CanPoll()) {
            all_pollable = false;
        }
    }
    if (!all_pollable) {
        LOG(ERROR) << "IIO device " << info_.dev_num
                   << ": buffer delivers no data and sensors cannot be polled; no data available";
        mode_ = Mode::kIdle;
        return;
    }
    mode_ = Mode::kPoll;
    for (auto& sensor : sensors_) {
        if (sensor->IsActive()) {
            sensor->ResetFilterState();
            StartPollLocked(sensor.get());
        }
    }
    LOG(WARNING) << "IIO device " << info_.dev_num << ": switched to poll mode";
}

void IioDevice::ReaderThread() {
    pthread_setname_np(pthread_self(), ("iio-rd" + std::to_string(info_.dev_num)).c_str());
    LOG(DEBUG) << "IIO device " << info_.dev_num << ": reader thread started";

    const int fd = fd_.get();
    const int wake_fd = wake_pipe_read_.get();
    const size_t scan_size = scan_size_;
    std::vector<uint8_t> buffer(scan_size * kBufferLength);
    std::vector<uint8_t> partial;

    struct pollfd fds[2];
    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[1].fd = wake_fd;
    fds[1].events = POLLIN;

    int64_t last_data_ns = GetBootTimeNs();
    bool failed = false;
    while (!reader_stop_.load()) {
        fds[0].revents = 0;
        fds[1].revents = 0;
        int ret = poll(fds, 2, kPollTimeoutMs);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG(ERROR) << "IIO device " << info_.dev_num << ": poll() failed: " << strerror(errno);
            failed = true;
            break;
        }
        if (fds[1].revents & POLLIN) {
            break;
        }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOG(WARNING) << "IIO device " << info_.dev_num << ": buffer fd error";
            failed = true;
            break;
        }
        if (fds[0].revents & POLLIN) {
            ssize_t bytes = TEMP_FAILURE_RETRY(read(fd, buffer.data(), buffer.size()));
            if (bytes < 0) {
                if (errno == EAGAIN) {
                    continue;
                }
                LOG(ERROR) << "IIO device " << info_.dev_num
                           << ": read() failed: " << strerror(errno);
                failed = true;
                break;
            }
            if (bytes == 0) {
                continue;
            }
            last_data_ns = GetBootTimeNs();
            // Reads return whole scans, but be defensive.
            const uint8_t* data = buffer.data();
            size_t available = static_cast<size_t>(bytes);
            if (!partial.empty()) {
                partial.insert(partial.end(), data, data + available);
                data = partial.data();
                available = partial.size();
            }
            const size_t count = available / scan_size;
            if (count > 0) {
                HandleScans(data, count);
            }
            std::vector<uint8_t> leftover(data + count * scan_size, data + available);
            partial = std::move(leftover);
            continue;
        }
        // Timeout: watchdog.
        int64_t timeout_ns;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            timeout_ns = WatchdogTimeoutNsLocked();
        }
        if (GetBootTimeNs() - last_data_ns > timeout_ns) {
            LOG(WARNING) << "IIO device " << info_.dev_num << ": no buffer data for "
                         << (GetBootTimeNs() - last_data_ns) / 1000000 << " ms";
            failed = true;
            break;
        }
    }

    if (failed && !reader_stop_.load()) {
        FallbackToPollFromReader();
    }
    LOG(DEBUG) << "IIO device " << info_.dev_num << ": reader thread stopped";
}

}  // namespace aidl::android::hardware::sensors::mainline
