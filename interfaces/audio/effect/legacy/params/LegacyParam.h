/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include <system/audio_effect.h>

#include "LegacyLibrary.h"

namespace aidl::android::hardware::audio::effect::legacy {

// Byte buffer laid out as an effect_param_t (header, padded parameter area,
// value area) as expected by EFFECT_CMD_SET_PARAM / EFFECT_CMD_GET_PARAM.
class LegacyParam {
  public:
    // Starts a parameter with `psize` bytes of parameter data and room for
    // `vsize` bytes of value.
    LegacyParam(uint32_t psize, uint32_t vsize);
    // Wraps an existing serialized effect_param_t (e.g. from a
    // DefaultExtension). Invalid input yields an empty buffer.
    explicit LegacyParam(const std::vector<uint8_t>& bytes);

    bool IsValid() const { return !bytes_.empty(); }
    effect_param_t* param() { return reinterpret_cast<effect_param_t*>(bytes_.data()); }
    const effect_param_t* param() const {
        return reinterpret_cast<const effect_param_t*>(bytes_.data());
    }
    uint32_t psize() const { return param()->psize; }
    uint32_t vsize() const { return param()->vsize; }
    // Total buffer size (header + padded psize + vsize).
    uint32_t size() const { return static_cast<uint32_t>(bytes_.size()); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    std::vector<uint8_t>& mutable_bytes() { return bytes_; }

    // Parameter area accessors (indices in units of T).
    template <typename T>
    void SetParam(size_t index, T value) {
        std::memcpy(param()->data + index * sizeof(T), &value, sizeof(T));
    }
    template <typename T>
    T GetParam(size_t index) const {
        T value{};
        std::memcpy(&value, param()->data + index * sizeof(T), sizeof(T));
        return value;
    }
    // Value area accessors (byte offset within the value).
    template <typename T>
    void SetValue(size_t byte_offset, T value) {
        std::memcpy(ValueData() + byte_offset, &value, sizeof(T));
    }
    template <typename T>
    T GetValue(size_t byte_offset) const {
        T value{};
        std::memcpy(&value, ValueData() + byte_offset, sizeof(T));
        return value;
    }

    static uint32_t Padded(uint32_t size) { return (size + 3) & ~3u; }

  private:
    uint8_t* ValueData() { return reinterpret_cast<uint8_t*>(param()->data) + Padded(psize()); }
    const uint8_t* ValueData() const {
        return reinterpret_cast<const uint8_t*>(param()->data) + Padded(psize());
    }

    std::vector<uint8_t> bytes_;
};

// EFFECT_CMD_SET_PARAM with a fully populated LegacyParam. Returns the legacy
// status (0 on success).
int32_t SetParam(LegacyEffectHandle& effect, LegacyParam& param);

// EFFECT_CMD_GET_PARAM: `param` carries the request (parameter area filled,
// vsize = room for the value). On success the value area is filled in and
// vsize updated.
int32_t GetParam(LegacyEffectHandle& effect, LegacyParam* param);

// Common shapes: one uint32_t parameter id with a single value.
template <typename V>
int32_t SetSimple(LegacyEffectHandle& effect, uint32_t id, V value) {
    LegacyParam param(sizeof(uint32_t), sizeof(V));
    param.SetParam<uint32_t>(0, id);
    param.SetValue<V>(0, value);
    return SetParam(effect, param);
}

template <typename V>
std::optional<V> GetSimple(LegacyEffectHandle& effect, uint32_t id) {
    LegacyParam param(sizeof(uint32_t), sizeof(V));
    param.SetParam<uint32_t>(0, id);
    if (GetParam(effect, &param) != 0 || param.vsize() < sizeof(V)) return std::nullopt;
    return param.GetValue<V>(0);
}

// One uint32_t id plus one int32_t argument in the parameter area, single
// value (e.g. EQ_PARAM_BAND_LEVEL with the band index).
template <typename V>
int32_t SetIndexed(LegacyEffectHandle& effect, uint32_t id, int32_t index, V value) {
    LegacyParam param(2 * sizeof(uint32_t), sizeof(V));
    param.SetParam<uint32_t>(0, id);
    param.SetParam<int32_t>(1, index);
    param.SetValue<V>(0, value);
    return SetParam(effect, param);
}

template <typename V>
std::optional<V> GetIndexed(LegacyEffectHandle& effect, uint32_t id, int32_t index) {
    LegacyParam param(2 * sizeof(uint32_t), sizeof(V));
    param.SetParam<uint32_t>(0, id);
    param.SetParam<int32_t>(1, index);
    if (GetParam(effect, &param) != 0 || param.vsize() < sizeof(V)) return std::nullopt;
    return param.GetValue<V>(0);
}

}  // namespace aidl::android::hardware::audio::effect::legacy
