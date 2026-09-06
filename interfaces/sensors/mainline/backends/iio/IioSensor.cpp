/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsIio"

#include "IioSensor.h"

#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/stringprintf.h>
#include <libsensors_common/SensorEvents.h>
#include <libsensors_common/SensorTypes.h>
#include <libsensors_common/Settings.h>
#include <libsensors_common/Sysfs.h>

#include <algorithm>
#include <cmath>

namespace aidl::android::hardware::sensors::mainline {

namespace {

constexpr int64_t kDefaultPeriodNs = 100LL * 1000 * 1000;
constexpr int32_t kOneSecondUs = 1000 * 1000;
// Fastest rates we are willing to serve: buffer mode is limited by the demux
// thread, poll mode by the cost of sysfs reads.
constexpr int32_t kMinDelayBufferUs = 5000;
constexpr int32_t kMinDelayPollUs = 20000;
// On-change sensors are polled at most this fast.
constexpr int64_t kMinOnChangePollPeriodNs = 66LL * 1000 * 1000;
constexpr float kProximityNearCm = 0.0f;
constexpr float kProximityFarCm = 5.0f;
constexpr float kMaxLightRange = 200000.0f;
// iio-sensor-proxy style hysteresis around the near level.
constexpr double kProximityLowWaterMark = 0.9;
constexpr double kProximityHighWaterMark = 1.1;

}  // namespace

IioSensor::IioSensor(const IioDeviceInfo& device, int32_t handle, const IioSensorSpec& spec,
                     std::vector<IioChannel*> channels, const SensorHwdb* hwdb)
    : device_(device),
      spec_(spec),
      channels_(std::move(channels)),
      hwdb_(hwdb),
      period_ns_(kDefaultPeriodNs) {
    info_.sensorHandle = handle;
    info_.type = spec_.android_type;
    ApplySensorTypeDefaults(&info_);
    info_.version = 1;

    can_buffer_ = device_.buffer_capable &&
                  std::all_of(channels_.begin(), channels_.end(),
                              [](const IioChannel* c) { return c->has_scan_element; });
    can_poll_ = std::all_of(channels_.begin(), channels_.end(),
                            [](const IioChannel* c) { return !c->poll_attribute.empty(); });

    auto traits = GetSensorTypeTraits(spec_.android_type);
    info_.name = device_.model + " " +
                 (traits ? std::string(traits->label) : toString(spec_.android_type));

    std::string vendor = VendorFromCompatible(device_.compatible);
    if (vendor.empty()) {
        vendor = VendorFromModalias(device_.modalias);
    }
    info_.vendor = vendor.empty() ? "Linux IIO" : vendor;

    DeriveSensorInfo();
    if (spec_.payload == IioSensorSpec::Payload::kVec3) {
        ResolveMountMatrix();
    }
    if (spec_.payload == IioSensorSpec::Payload::kProximity) {
        ResolveProximityNearLevel();
    }
    ApplyConfigOverrides();
}

std::vector<std::string> IioSensor::SettingKeys(const std::string& key) const {
    // Most specific first: per sensor type on the device, then per device;
    // both for the model based key and for the raw device name.
    const std::string type_name = ConfigNameForSensorType(spec_.android_type);
    std::vector<std::string> keys = {
            device_.config_key + "." + type_name + "." + key,
            device_.config_key + "." + key,
    };
    if (!device_.alt_config_key.empty()) {
        keys.push_back(device_.alt_config_key + "." + type_name + "." + key);
        keys.push_back(device_.alt_config_key + "." + key);
    }
    return keys;
}

std::optional<std::string> IioSensor::GetSetting(const std::string& key) const {
    return Settings::Get().GetFirstString(SettingKeys(key));
}

void IioSensor::DeriveSensorInfo() {
    const bool is_continuous = GetReportingMode(info_.flags) == ReportingMode::kContinuous;

    // Range and resolution from scale and bit width.
    float max_range = 0.0f;
    float resolution = 0.0f;
    for (const IioChannel* channel : channels_) {
        if (!channel->has_scale && channel->poll_is_processed) {
            continue;
        }
        const double unit_scale = std::fabs(channel->scale * spec_.unit_factor);
        if (channel->has_scan_element && channel->scan_type.realbits < 31) {
            const double max_raw = channel->scan_type.MaxRawValue();
            const double offset = device_.quirks.ignore_offset ? 0.0 : channel->offset;
            max_range = std::max(max_range, static_cast<float>((max_raw + offset) * unit_scale));
        }
        if (channel->has_scale) {
            resolution = resolution == 0.0f ? static_cast<float>(unit_scale)
                                            : std::min(resolution, static_cast<float>(unit_scale));
        }
    }
    if (spec_.payload == IioSensorSpec::Payload::kProximity) {
        // Binary near/far unless the driver reports distances.
        info_.maxRange = kProximityFarCm;
        info_.resolution = kProximityFarCm;
        if (device_.quirks.proximity_is_distance && !channels_.empty()) {
            // qcom_smgr: "offset" holds the range in metres.
            double range_m = channels_[0]->offset;
            if (range_m > 0.0) {
                info_.maxRange = static_cast<float>(range_m * 100.0);
                info_.resolution = static_cast<float>(std::fabs(channels_[0]->scale) * 100.0);
            }
        }
    } else {
        if (max_range > 0.0f) {
            info_.maxRange = max_range;
        }
        if (resolution > 0.0f) {
            info_.resolution = resolution;
        }
        if (spec_.android_type == SensorType::LIGHT) {
            info_.maxRange = std::min(info_.maxRange, kMaxLightRange);
        }
        if (spec_.payload == IioSensorSpec::Payload::kQuaternion) {
            info_.maxRange = 1.0f;
        }
    }

    // Sampling rates.
    if (is_continuous) {
        std::vector<double> frequencies;
        const std::vector<std::string> candidates = {
                device_.sysfs_path + "/in_" + spec_.iio_type + "_sampling_frequency_available",
                device_.sysfs_path + "/sampling_frequency_available",
                device_.sysfs_path + "/buffer/sampling_frequency_available",
        };
        for (const auto& path : candidates) {
            auto content = sysfs::ReadString(path);
            if (content.has_value()) {
                frequencies = sysfs::ParseDoubleList(*content);
                if (!frequencies.empty()) {
                    LOG(DEBUG) << info_.name << ": available frequencies from " << path << ": "
                               << *content;
                    break;
                }
            }
        }
        const int32_t min_delay_floor = can_buffer_ ? kMinDelayBufferUs : kMinDelayPollUs;
        if (!frequencies.empty() && frequencies.back() > 0.0) {
            info_.minDelayUs = static_cast<int32_t>(1.0e6 / frequencies.back());
            if (frequencies.front() > 0.0) {
                info_.maxDelayUs = static_cast<int32_t>(1.0e6 / frequencies.front());
            }
        }
        info_.minDelayUs = std::max(info_.minDelayUs, min_delay_floor);
        info_.maxDelayUs = std::min(info_.maxDelayUs, kOneSecondUs);
        info_.maxDelayUs = std::max(info_.maxDelayUs, info_.minDelayUs);
    }
}

void IioSensor::ResolveMountMatrix() {
    std::string source = "identity";
    std::optional<std::string> text = GetSetting("mount_matrix");
    if (text.has_value()) {
        source = "configuration";
    }
    if (!text.has_value() && hwdb_ != nullptr && !device_.modalias.empty()) {
        text = hwdb_->GetMountMatrix(device_.modalias, device_.label);
        if (text.has_value()) {
            source = "hwdb";
        }
    }
    if (!text.has_value()) {
        const std::vector<std::string> candidates = {
                device_.sysfs_path + "/in_" + spec_.iio_type + "_mount_matrix",
                device_.sysfs_path + "/in_mount_matrix",
                device_.sysfs_path + "/mount_matrix",
        };
        for (const auto& path : candidates) {
            auto content = sysfs::ReadString(path);
            if (content.has_value() && !content->empty()) {
                text = content;
                source = path.substr(device_.sysfs_path.size() + 1);
                break;
            }
        }
    }
    if (text.has_value()) {
        auto matrix = MountMatrix::Parse(*text);
        if (matrix.has_value()) {
            mount_matrix_ = *matrix;
        } else {
            LOG(WARNING) << info_.name << ": invalid mount matrix '" << *text << "' from " << source
                         << ", using identity";
            source = "identity (invalid " + source + ")";
        }
    }
    LOG(INFO) << info_.name << ": mount matrix [" << mount_matrix_.ToString() << "] from "
              << source;
}

void IioSensor::ResolveProximityNearLevel() {
    if (device_.quirks.proximity_is_distance) {
        LOG(INFO) << info_.name << ": proximity reports distances";
        return;
    }
    std::string source;
    auto configured = Settings::Get().GetFirstDouble(SettingKeys("proximity_near_level"));
    if (configured.has_value() && *configured > 0.0) {
        proximity_near_level_ = *configured;
        source = "configuration";
    }
    if (proximity_near_level_ <= 0.0 && hwdb_ != nullptr && !device_.modalias.empty()) {
        auto level = hwdb_->GetProximityNearLevel(device_.modalias, device_.label);
        if (level.has_value() && *level > 0) {
            proximity_near_level_ = static_cast<double>(*level);
            source = "hwdb";
        }
    }
    if (proximity_near_level_ <= 0.0 && !channels_.empty()) {
        // Device tree "proximity-near-level" exposed by the driver.
        auto level =
                sysfs::ReadDouble(device_.sysfs_path + "/" + channels_[0]->id.Key() + "_nearlevel");
        if (!level.has_value()) {
            level = sysfs::ReadDouble(device_.sysfs_path + "/in_proximity_nearlevel");
        }
        if (level.has_value() && *level > 0.0) {
            proximity_near_level_ = *level;
            source = "sysfs nearlevel";
        }
    }
    if (proximity_near_level_ <= 0.0 && device_.quirks.proximity_near_level.has_value()) {
        proximity_near_level_ = *device_.quirks.proximity_near_level;
        source = "driver quirk";
    }
    if (proximity_near_level_ <= 0.0 && !channels_.empty() && channels_[0]->has_scan_element) {
        proximity_near_level_ = channels_[0]->scan_type.MaxRawValue() / 2.0;
        source = "half scale fallback";
    }
    if (proximity_near_level_ <= 0.0) {
        LOG(WARNING) << info_.name
                     << ": unknown proximity near level, the sensor will always report 'far'. Set "
                     << device_.config_key << ".proximity_near_level or add a hwdb entry.";
        return;
    }
    LOG(INFO) << info_.name << ": proximity near level " << proximity_near_level_ << " (" << source
              << ")";
}

void IioSensor::ApplyConfigOverrides() {
    Settings& settings = Settings::Get();
    auto name = GetSetting("name");
    if (name.has_value() && !name->empty()) {
        info_.name = *name;
    }
    auto vendor = GetSetting("vendor");
    if (vendor.has_value() && !vendor->empty()) {
        info_.vendor = *vendor;
    }
    auto power = settings.GetFirstDouble(SettingKeys("power"));
    if (power.has_value() && *power >= 0.0) {
        info_.power = static_cast<float>(*power);
    }
    auto max_range = settings.GetFirstDouble(SettingKeys("max_range"));
    if (max_range.has_value() && *max_range > 0.0) {
        info_.maxRange = static_cast<float>(*max_range);
    }
    auto resolution = settings.GetFirstDouble(SettingKeys("resolution"));
    if (resolution.has_value() && *resolution > 0.0) {
        info_.resolution = static_cast<float>(*resolution);
    }
    auto min_delay = settings.GetFirstInt(SettingKeys("min_delay_us"));
    if (min_delay.has_value()) {
        info_.minDelayUs = static_cast<int32_t>(*min_delay);
    }
    auto max_delay = settings.GetFirstInt(SettingKeys("max_delay_us"));
    if (max_delay.has_value()) {
        info_.maxDelayUs = static_cast<int32_t>(*max_delay);
    }
    auto wake_up = settings.GetFirstBool(SettingKeys("wake_up"));
    if (wake_up.has_value()) {
        if (*wake_up) {
            info_.flags |= SensorInfo::SENSOR_FLAG_BITS_WAKE_UP;
        } else {
            info_.flags &= ~SensorInfo::SENSOR_FLAG_BITS_WAKE_UP;
        }
    }
}

void IioSensor::SetActive(bool active) {
    active_.store(active);
}

void IioSensor::SetPeriodNs(int64_t period_ns) {
    period_ns_.store(period_ns > 0 ? period_ns : kDefaultPeriodNs);
}

double IioSensor::GetRequestedFrequencyHz() const {
    return 1.0e9 / static_cast<double>(period_ns_.load());
}

int64_t IioSensor::GetPollPeriodNs() const {
    if (GetReportingMode(info_.flags) == ReportingMode::kContinuous) {
        return period_ns_.load();
    }
    return std::max(period_ns_.load(), kMinOnChangePollPeriodNs);
}

double IioSensor::ConvertChannelValue(const IioChannel& channel, double raw) const {
    const double offset = device_.quirks.ignore_offset ? 0.0 : channel.offset;
    if (spec_.payload == IioSensorSpec::Payload::kProximity) {
        if (device_.quirks.proximity_is_distance) {
            // Distance in metres, see IioDeviceQuirks.
            return raw * channel.scale + offset;
        }
        // Near level comparison is done on raw counts.
        return raw;
    }
    return (raw + offset) * channel.scale * spec_.unit_factor;
}

float IioSensor::ConvertProximity(double value) {
    if (device_.quirks.proximity_is_distance) {
        double cm = value * 100.0;
        return static_cast<float>(std::clamp(cm, 0.0, static_cast<double>(info_.maxRange)));
    }
    if (proximity_near_level_ <= 0.0) {
        return kProximityFarCm;
    }
    const double threshold = proximity_near_level_ * (proximity_is_near_ ? kProximityLowWaterMark
                                                                         : kProximityHighWaterMark);
    proximity_is_near_ = value > threshold;
    return proximity_is_near_ ? kProximityNearCm : kProximityFarCm;
}

std::optional<Event> IioSensor::BuildEvent(const std::vector<double>& values,
                                           int64_t timestamp_ns) {
    const int32_t handle = info_.sensorHandle;
    switch (spec_.payload) {
        case IioSensorSpec::Payload::kVec3: {
            if (values.size() < 3) return std::nullopt;
            float x = static_cast<float>(values[0]);
            float y = static_cast<float>(values[1]);
            float z = static_cast<float>(values[2]);
            mount_matrix_.Apply(&x, &y, &z);
            return MakeVec3Event(handle, info_.type, timestamp_ns, x, y, z);
        }
        case IioSensorSpec::Payload::kScalar:
            if (values.empty()) return std::nullopt;
            return MakeScalarEvent(handle, info_.type, timestamp_ns, static_cast<float>(values[0]));
        case IioSensorSpec::Payload::kProximity:
            if (values.empty()) return std::nullopt;
            return MakeScalarEvent(handle, info_.type, timestamp_ns, ConvertProximity(values[0]));
        case IioSensorSpec::Payload::kQuaternion: {
            if (values.size() < 4) return std::nullopt;
            const float x = static_cast<float>(values[0]);
            const float y = static_cast<float>(values[1]);
            const float z = static_cast<float>(values[2]);
            const float w = static_cast<float>(values[3]);
            if (info_.type == SensorType::GAME_ROTATION_VECTOR) {
                return MakeVec4Event(handle, info_.type, timestamp_ns, x, y, z, w);
            }
            return MakeRotationVectorEvent(handle, info_.type, timestamp_ns, x, y, z, w,
                                           -1.0f /* accuracy unknown */);
        }
        case IioSensorSpec::Payload::kStepCount:
            if (values.empty()) return std::nullopt;
            return MakeStepCountEvent(handle, timestamp_ns, static_cast<int64_t>(values[0]));
    }
    return std::nullopt;
}

std::optional<Event> IioSensor::BuildEventFromScan(const uint8_t* scan, int64_t timestamp_ns) {
    std::vector<double> values;
    for (const IioChannel* channel : channels_) {
        if (!channel->enabled_in_scan) {
            return std::nullopt;
        }
        if (spec_.payload == IioSensorSpec::Payload::kQuaternion) {
            const int elements = std::min(channel->scan_type.repeat, 4);
            for (int i = 0; i < elements; i++) {
                values.push_back(ConvertChannelValue(
                        *channel, static_cast<double>(channel->DecodeRaw(scan, i))));
            }
        } else {
            values.push_back(
                    ConvertChannelValue(*channel, static_cast<double>(channel->DecodeRaw(scan))));
        }
    }
    return BuildEvent(values, timestamp_ns);
}

std::optional<Event> IioSensor::ReadEventFromSysfs(int64_t timestamp_ns) {
    std::vector<double> values;
    for (const IioChannel* channel : channels_) {
        if (channel->poll_attribute.empty()) {
            return std::nullopt;
        }
        const std::string path = device_.sysfs_path + "/" + channel->poll_attribute;
        auto content = sysfs::ReadString(path);
        if (!content.has_value()) {
            LOG(WARNING) << info_.name << ": failed to read " << path;
            return std::nullopt;
        }
        if (spec_.payload == IioSensorSpec::Payload::kQuaternion) {
            // "x y z w" from read_raw_multi.
            for (double v : sysfs::ParseDoubleList(*content)) {
                values.push_back(ConvertChannelValue(*channel, v));
            }
            continue;
        }
        double raw = 0.0;
        if (!::android::base::ParseDouble(content->c_str(), &raw)) {
            LOG(WARNING) << info_.name << ": unparsable value '" << *content << "' in " << path;
            return std::nullopt;
        }
        if (channel->poll_is_processed) {
            // "_input" values are already in IIO units.
            values.push_back(spec_.payload == IioSensorSpec::Payload::kProximity
                                     ? raw
                                     : raw * spec_.unit_factor);
        } else {
            values.push_back(ConvertChannelValue(*channel, raw));
        }
    }
    return BuildEvent(values, timestamp_ns);
}

std::optional<Event> IioSensor::Filter(const Event& event) {
    if (GetReportingMode(info_.flags) == ReportingMode::kContinuous) {
        // Decimate to the requested rate (the device may run faster because it
        // is shared with another sensor).
        const int64_t period = period_ns_.load();
        if (last_emit_ns_ != 0 && event.timestamp - last_emit_ns_ < period - period / 10) {
            return std::nullopt;
        }
    } else if (last_event_.has_value() && HaveSamePayload(*last_event_, event)) {
        return std::nullopt;
    }
    last_emit_ns_ = event.timestamp;
    last_event_ = event;
    return event;
}

void IioSensor::ResetFilterState() {
    last_emit_ns_ = 0;
    last_event_.reset();
    proximity_is_near_ = false;
}

std::string IioSensor::Describe() const {
    std::string channels;
    for (const IioChannel* channel : channels_) {
        if (!channels.empty()) channels += ",";
        channels += channel->id.Key();
    }
    return ::android::base::StringPrintf("%s [iio:device%d channels=%s buffer=%d poll=%d]",
                                         SensorInfoToString(info_).c_str(), device_.dev_num,
                                         channels.c_str(), can_buffer_, can_poll_);
}

}  // namespace aidl::android::hardware::sensors::mainline
