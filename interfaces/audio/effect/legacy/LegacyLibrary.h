/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <hardware/audio_effect.h>

namespace aidl::android::hardware::audio::effect::legacy {

class LegacyLibrary;

// An instance created by a legacy effect library (effect_handle_t), owned by
// the wrapper. Released together with the object.
class LegacyEffectHandle {
  public:
    ~LegacyEffectHandle();
    LegacyEffectHandle(const LegacyEffectHandle&) = delete;
    LegacyEffectHandle& operator=(const LegacyEffectHandle&) = delete;

    effect_handle_t handle() const { return handle_; }
    const effect_descriptor_t& descriptor() const { return descriptor_; }
    const std::string& library_path() const;

    // effect_interface_s::command(). Returns the status of the command;
    // `reply_size` is updated with the size actually written.
    int32_t Command(uint32_t cmd, uint32_t cmd_size, void* cmd_data, uint32_t* reply_size,
                    void* reply_data);
    // Convenience for commands with a single int32_t status reply.
    int32_t CommandWithStatusReply(uint32_t cmd, uint32_t cmd_size, void* cmd_data);
    // effect_interface_s::process().
    int32_t Process(audio_buffer_t* in, audio_buffer_t* out);

  private:
    friend class LegacyLibrary;
    LegacyEffectHandle(std::shared_ptr<LegacyLibrary> library, effect_handle_t handle,
                       const effect_descriptor_t& descriptor);

    // Keeps the library mapped for as long as an instance exists.
    const std::shared_ptr<LegacyLibrary> library_;
    effect_handle_t handle_;
    const effect_descriptor_t descriptor_;
};

// A dlopen()ed legacy effect library exposing AUDIO_EFFECT_LIBRARY_INFO_SYM
// (hardware/audio_effect.h). Instances are shared between the factory and
// every effect created from the library.
class LegacyLibrary : public std::enable_shared_from_this<LegacyLibrary> {
  public:
    // Loads `path` and validates the library tag and API version. Returns
    // nullptr on failure.
    static std::shared_ptr<LegacyLibrary> Open(const std::string& name, const std::string& path);
    ~LegacyLibrary();

    LegacyLibrary(const LegacyLibrary&) = delete;
    LegacyLibrary& operator=(const LegacyLibrary&) = delete;

    const std::string& name() const { return name_; }
    const std::string& path() const { return path_; }
    const audio_effect_library_t& info() const { return *info_; }

    // audio_effect_library_t::get_descriptor().
    std::optional<effect_descriptor_t> GetDescriptor(const effect_uuid_t& uuid);
    // audio_effect_library_t::create_effect(). Returns nullptr on failure.
    std::unique_ptr<LegacyEffectHandle> CreateEffect(const effect_uuid_t& uuid, int32_t session_id,
                                                     int32_t io_id);

  private:
    friend class LegacyEffectHandle;
    LegacyLibrary(std::string name, std::string path, void* dl_handle,
                  const audio_effect_library_t* info);
    void ReleaseEffect(effect_handle_t handle);

    const std::string name_;
    const std::string path_;
    void* dl_handle_;
    const audio_effect_library_t* const info_;
};

// Human readable "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
std::string UuidToString(const effect_uuid_t& uuid);
bool UuidEquals(const effect_uuid_t& a, const effect_uuid_t& b);

}  // namespace aidl::android::hardware::audio::effect::legacy
