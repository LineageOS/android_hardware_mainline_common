/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <aidl/android/hardware/camera/device/CameraMetadata.h>
#include <system/camera_metadata.h>

namespace aidl::android::hardware::camera::mainline {

// Owning wrapper around a camera_metadata_t buffer that grows as needed.
class Metadata {
  public:
    Metadata();
    Metadata(const Metadata& other);
    Metadata& operator=(const Metadata& other);
    Metadata(Metadata&&) = default;
    Metadata& operator=(Metadata&&) = default;

    // Copies and validates a buffer received over binder. Returns nullopt for
    // a malformed buffer. An empty buffer gives empty metadata.
    static std::optional<Metadata> FromAidl(const device::CameraMetadata& aidl);
    // Compact copy for sending over binder.
    device::CameraMetadata ToAidl() const;

    // Sets `tag` to `count` values of the tag's type. Returns false (and
    // logs) when the tag is unknown or the type does not match.
    bool Set(uint32_t tag, const void* data, size_t count, uint8_t type);

    bool Set(uint32_t tag, const std::vector<uint8_t>& values);
    bool Set(uint32_t tag, const std::vector<int32_t>& values);
    bool Set(uint32_t tag, const std::vector<int64_t>& values);
    bool Set(uint32_t tag, const std::vector<float>& values);
    bool Set(uint32_t tag, const std::vector<double>& values);
    bool Set(uint32_t tag, const std::vector<camera_metadata_rational_t>& values);

    bool SetU8(uint32_t tag, uint8_t value) { return Set(tag, std::vector<uint8_t>{value}); }
    bool SetI32(uint32_t tag, int32_t value) { return Set(tag, std::vector<int32_t>{value}); }
    bool SetI64(uint32_t tag, int64_t value) { return Set(tag, std::vector<int64_t>{value}); }
    bool SetFloat(uint32_t tag, float value) { return Set(tag, std::vector<float>{value}); }

    void Erase(uint32_t tag);
    // Copies every entry of `other`, replacing existing ones.
    void Merge(const Metadata& other);

    std::optional<camera_metadata_ro_entry_t> Find(uint32_t tag) const;
    bool Has(uint32_t tag) const { return Find(tag).has_value(); }

    // First value of a tag, when present with the expected type.
    std::optional<uint8_t> GetU8(uint32_t tag) const;
    std::optional<int32_t> GetI32(uint32_t tag) const;
    std::optional<int64_t> GetI64(uint32_t tag) const;
    std::optional<float> GetFloat(uint32_t tag) const;
    std::optional<double> GetDouble(uint32_t tag) const;
    std::vector<int32_t> GetI32s(uint32_t tag) const;

    // All tags present, in storage order.
    std::vector<int32_t> Tags() const;
    size_t EntryCount() const;
    bool Empty() const { return EntryCount() == 0; }

    const camera_metadata_t* Raw() const { return buffer_.get(); }

  private:
    struct Deleter {
        void operator()(camera_metadata_t* metadata) const { free_camera_metadata(metadata); }
    };
    using Buffer = std::unique_ptr<camera_metadata_t, Deleter>;

    explicit Metadata(Buffer buffer) : buffer_(std::move(buffer)) {}

    // Makes room for one more entry with `data_bytes` of data.
    bool Reserve(size_t data_bytes);

    Buffer buffer_;
};

}  // namespace aidl::android::hardware::camera::mainline
