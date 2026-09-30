/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Properties"

#include "Properties.h"

#include <algorithm>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/parsebool.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

std::string Key(const char* suffix) {
    return std::string(Properties::kPrefix) + suffix;
}

std::optional<bool> ParseOptionalBool(const std::string& value) {
    switch (::android::base::ParseBool(value)) {
        case ::android::base::ParseBoolResult::kTrue:
            return true;
        case ::android::base::ParseBoolResult::kFalse:
            return false;
        case ::android::base::ParseBoolResult::kError:
            break;
    }
    return std::nullopt;
}

std::optional<Facing> ParseFacing(const std::string& value) {
    if (value == "back" || value == "rear") return Facing::kBack;
    if (value == "front") return Facing::kFront;
    return std::nullopt;
}

std::optional<int> ParseRotation(const std::string& value) {
    int rotation;
    if (!::android::base::ParseInt(value, &rotation)) return std::nullopt;
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) return std::nullopt;
    return rotation;
}

// Sets `field` from the property `key` of the selector at `prefix` with
// `parse`, unless a more specific selector already set it.
template <typename T>
void Merge(std::optional<T>* field, const std::string& prefix, const char* key,
           const std::function<std::string(const std::string&)>& get,
           std::optional<T> (*parse)(const std::string&)) {
    if (field->has_value()) return;
    const std::string name = prefix + key;
    const std::string value = get(name);
    if (value.empty()) return;
    *field = parse(value);
    if (!field->has_value()) {
        LOG(WARNING) << "ignoring invalid value \"" << value << "\" of " << name;
    }
}

}  // namespace

Properties Properties::Load() {
    using ::android::base::GetBoolProperty;
    using ::android::base::GetIntProperty;

    Properties props;
    props.default_internal = GetBoolProperty(Key("default_internal"), props.default_internal);
    props.wait_internal_count = std::clamp(
            GetIntProperty(Key("wait_internal_count"), props.wait_internal_count), 0, 64);
    props.wait_internal_ms =
            std::clamp(GetIntProperty(Key("wait_internal_ms"), props.wait_internal_ms), 0, 60000);
    props.external_id_offset = std::clamp(
            GetIntProperty(Key("external_id_offset"), props.external_id_offset), 1, 100000);
    props.verbose_logging = GetBoolProperty(Key("log.verbose"), props.verbose_logging);

    LOG(INFO) << "loaded properties: " << props.ToString();
    return props;
}

std::string Properties::ToString() const {
    std::ostringstream os;
    os << "default_internal=" << default_internal << " wait_internal_count=" << wait_internal_count
       << " wait_internal_ms=" << wait_internal_ms << " external_id_offset=" << external_id_offset
       << " log.verbose=" << verbose_logging;
    return os.str();
}

Properties::DeviceProperties Properties::LoadDeviceProperties(
        const std::vector<std::string>& selectors,
        const std::function<std::string(const std::string&)>& get) {
    DeviceProperties merged;
    for (const std::string& selector : selectors) {
        if (selector.empty()) continue;
        const std::string prefix = std::string(kPrefix) + "device." + selector + ".";
        Merge(&merged.enabled, prefix, "enabled", get, ParseOptionalBool);
        Merge(&merged.internal, prefix, "internal", get, ParseOptionalBool);
        Merge(&merged.facing, prefix, "facing", get, ParseFacing);
        Merge(&merged.rotation, prefix, "rotation", get, ParseRotation);
    }
    return merged;
}

Properties::DeviceProperties Properties::LoadDeviceProperties(
        const std::vector<std::string>& selectors) {
    return LoadDeviceProperties(selectors, [](const std::string& name) {
        return ::android::base::GetProperty(name, "");
    });
}

std::string Properties::SanitizeSelector(const std::string& value) {
    std::string result = value;
    std::replace_if(
            result.begin(), result.end(),
            [](char c) {
                return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                         (c >= 'A' && c <= 'Z') || c == ':' || c == '@' || c == '-');
            },
            '_');
    return result;
}

}  // namespace aidl::android::hardware::camera::mainline
