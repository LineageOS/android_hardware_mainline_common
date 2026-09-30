/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Metadata"

#include "utils/Metadata.h"

#include <algorithm>
#include <cstring>

#include <android-base/logging.h>

namespace aidl::android::hardware::camera::mainline {

namespace {

// Return value of the camera_metadata functions on success.
constexpr int kOk = 0;

constexpr size_t kInitialEntries = 32;
constexpr size_t kInitialData = 512;

const char* TagName(uint32_t tag) {
    const char* name = get_camera_metadata_tag_name(tag);
    return name != nullptr ? name : "(unknown)";
}

}  // namespace

Metadata::Metadata() : buffer_(allocate_camera_metadata(kInitialEntries, kInitialData)) {
    CHECK(buffer_ != nullptr) << "failed to allocate camera metadata";
}

Metadata::Metadata(const Metadata& other) : buffer_(clone_camera_metadata(other.buffer_.get())) {
    CHECK(buffer_ != nullptr) << "failed to copy camera metadata";
}

Metadata& Metadata::operator=(const Metadata& other) {
    if (this != &other) {
        buffer_.reset(clone_camera_metadata(other.buffer_.get()));
        CHECK(buffer_ != nullptr) << "failed to copy camera metadata";
    }
    return *this;
}

std::optional<Metadata> Metadata::FromAidl(const device::CameraMetadata& aidl) {
    if (aidl.metadata.empty()) return Metadata();

    // The binder buffer is not necessarily aligned; work on an aligned copy.
    std::vector<uint64_t> aligned((aidl.metadata.size() + sizeof(uint64_t) - 1) / sizeof(uint64_t));
    memcpy(aligned.data(), aidl.metadata.data(), aidl.metadata.size());
    const auto* raw = reinterpret_cast<const camera_metadata_t*>(aligned.data());
    size_t size = aidl.metadata.size();
    const int result = validate_camera_metadata_structure(raw, &size);
    if (result != kOk && result != CAMERA_METADATA_VALIDATION_SHIFTED) {
        LOG(ERROR) << "malformed camera metadata (" << aidl.metadata.size() << " bytes)";
        return std::nullopt;
    }
    Buffer buffer(clone_camera_metadata(raw));
    if (buffer == nullptr) {
        LOG(ERROR) << "failed to copy camera metadata";
        return std::nullopt;
    }
    return Metadata(std::move(buffer));
}

device::CameraMetadata Metadata::ToAidl() const {
    device::CameraMetadata aidl;
    Buffer compact(clone_camera_metadata(buffer_.get()));
    if (compact == nullptr) {
        LOG(ERROR) << "failed to compact camera metadata";
        return aidl;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(compact.get());
    aidl.metadata.assign(bytes, bytes + get_camera_metadata_size(compact.get()));
    return aidl;
}

bool Metadata::Reserve(size_t data_bytes) {
    camera_metadata_t* raw = buffer_.get();
    const size_t entries = get_camera_metadata_entry_count(raw);
    const size_t entry_capacity = get_camera_metadata_entry_capacity(raw);
    const size_t data = get_camera_metadata_data_count(raw);
    const size_t data_capacity = get_camera_metadata_data_capacity(raw);
    if (entries + 1 <= entry_capacity && data + data_bytes <= data_capacity) return true;

    Buffer grown(allocate_camera_metadata(std::max(entry_capacity * 2, entries + 1),
                                          std::max(data_capacity * 2, data + data_bytes)));
    if (grown == nullptr || append_camera_metadata(grown.get(), raw) != kOk) {
        LOG(ERROR) << "failed to grow camera metadata";
        return false;
    }
    buffer_ = std::move(grown);
    return true;
}

bool Metadata::Set(uint32_t tag, const void* data, size_t count, uint8_t type) {
    const int tag_type = get_camera_metadata_tag_type(tag);
    if (tag_type < 0) {
        LOG(ERROR) << "unknown metadata tag 0x" << std::hex << tag;
        return false;
    }
    if (tag_type != type) {
        LOG(ERROR) << "metadata tag " << TagName(tag) << " has type " << tag_type << ", not "
                   << static_cast<int>(type);
        return false;
    }
    if (!Reserve(calculate_camera_metadata_entry_data_size(type, count))) return false;

    camera_metadata_entry_t entry;
    int result;
    if (find_camera_metadata_entry(buffer_.get(), tag, &entry) == kOk) {
        result = update_camera_metadata_entry(buffer_.get(), entry.index, data, count, nullptr);
    } else {
        result = add_camera_metadata_entry(buffer_.get(), tag, data, count);
    }
    if (result != kOk) {
        LOG(ERROR) << "failed to set metadata tag " << TagName(tag);
        return false;
    }
    return true;
}

bool Metadata::Set(uint32_t tag, const std::vector<uint8_t>& values) {
    return Set(tag, values.data(), values.size(), TYPE_BYTE);
}

bool Metadata::Set(uint32_t tag, const std::vector<int32_t>& values) {
    return Set(tag, values.data(), values.size(), TYPE_INT32);
}

bool Metadata::Set(uint32_t tag, const std::vector<int64_t>& values) {
    return Set(tag, values.data(), values.size(), TYPE_INT64);
}

bool Metadata::Set(uint32_t tag, const std::vector<float>& values) {
    return Set(tag, values.data(), values.size(), TYPE_FLOAT);
}

bool Metadata::Set(uint32_t tag, const std::vector<double>& values) {
    return Set(tag, values.data(), values.size(), TYPE_DOUBLE);
}

bool Metadata::Set(uint32_t tag, const std::vector<camera_metadata_rational_t>& values) {
    return Set(tag, values.data(), values.size(), TYPE_RATIONAL);
}

void Metadata::Erase(uint32_t tag) {
    camera_metadata_entry_t entry;
    if (find_camera_metadata_entry(buffer_.get(), tag, &entry) == kOk) {
        delete_camera_metadata_entry(buffer_.get(), entry.index);
    }
}

void Metadata::Merge(const Metadata& other) {
    const camera_metadata_t* raw = other.Raw();
    const size_t count = get_camera_metadata_entry_count(raw);
    for (size_t i = 0; i < count; ++i) {
        camera_metadata_ro_entry_t entry;
        if (get_camera_metadata_ro_entry(raw, i, &entry) != kOk) continue;
        Set(entry.tag, entry.data.u8, entry.count, entry.type);
    }
}

std::optional<camera_metadata_ro_entry_t> Metadata::Find(uint32_t tag) const {
    camera_metadata_ro_entry_t entry;
    if (find_camera_metadata_ro_entry(buffer_.get(), tag, &entry) != kOk) return std::nullopt;
    return entry;
}

std::optional<uint8_t> Metadata::GetU8(uint32_t tag) const {
    auto entry = Find(tag);
    if (!entry.has_value() || entry->type != TYPE_BYTE || entry->count == 0) return std::nullopt;
    return entry->data.u8[0];
}

std::optional<int32_t> Metadata::GetI32(uint32_t tag) const {
    auto entry = Find(tag);
    if (!entry.has_value() || entry->type != TYPE_INT32 || entry->count == 0) return std::nullopt;
    return entry->data.i32[0];
}

std::optional<int64_t> Metadata::GetI64(uint32_t tag) const {
    auto entry = Find(tag);
    if (!entry.has_value() || entry->type != TYPE_INT64 || entry->count == 0) return std::nullopt;
    return entry->data.i64[0];
}

std::optional<float> Metadata::GetFloat(uint32_t tag) const {
    auto entry = Find(tag);
    if (!entry.has_value() || entry->type != TYPE_FLOAT || entry->count == 0) return std::nullopt;
    return entry->data.f[0];
}

std::optional<double> Metadata::GetDouble(uint32_t tag) const {
    auto entry = Find(tag);
    if (!entry.has_value() || entry->type != TYPE_DOUBLE || entry->count == 0) return std::nullopt;
    return entry->data.d[0];
}

std::vector<int32_t> Metadata::GetI32s(uint32_t tag) const {
    auto entry = Find(tag);
    if (!entry.has_value() || entry->type != TYPE_INT32) return {};
    return std::vector<int32_t>(entry->data.i32, entry->data.i32 + entry->count);
}

std::vector<int32_t> Metadata::Tags() const {
    std::vector<int32_t> tags;
    const size_t count = get_camera_metadata_entry_count(buffer_.get());
    for (size_t i = 0; i < count; ++i) {
        camera_metadata_ro_entry_t entry;
        if (get_camera_metadata_ro_entry(buffer_.get(), i, &entry) == kOk) {
            tags.push_back(static_cast<int32_t>(entry.tag));
        }
    }
    return tags;
}

size_t Metadata::EntryCount() const {
    return get_camera_metadata_entry_count(buffer_.get());
}

}  // namespace aidl::android::hardware::camera::mainline
