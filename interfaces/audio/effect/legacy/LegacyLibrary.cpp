/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Library"

#include "LegacyLibrary.h"

#include <dlfcn.h>

#include <cerrno>
#include <cstring>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

namespace aidl::android::hardware::audio::effect::legacy {

namespace {

// EFFECT_API_VERSION_MINOR in hardware/audio_effect.h is broken (it refers
// to an undefined `m`), so spell it out.
uint32_t ApiVersionMinor(uint32_t version) {
    return version & 0xFFFF;
}

}  // namespace

// --- helpers -----------------------------------------------------------------

std::string UuidToString(const effect_uuid_t& uuid) {
    return ::android::base::StringPrintf("%08x-%04x-%04x-%04x-%02x%02x%02x%02x%02x%02x",
                                         uuid.timeLow, uuid.timeMid, uuid.timeHiAndVersion,
                                         uuid.clockSeq, uuid.node[0], uuid.node[1], uuid.node[2],
                                         uuid.node[3], uuid.node[4], uuid.node[5]);
}

bool UuidEquals(const effect_uuid_t& a, const effect_uuid_t& b) {
    return std::memcmp(&a, &b, sizeof(effect_uuid_t)) == 0;
}

// --- LegacyEffectHandle ------------------------------------------------------

LegacyEffectHandle::LegacyEffectHandle(std::shared_ptr<LegacyLibrary> library,
                                       effect_handle_t handle,
                                       const effect_descriptor_t& descriptor)
    : library_(std::move(library)), handle_(handle), descriptor_(descriptor) {}

LegacyEffectHandle::~LegacyEffectHandle() {
    if (handle_ != nullptr) {
        library_->ReleaseEffect(handle_);
        handle_ = nullptr;
    }
}

const std::string& LegacyEffectHandle::library_path() const {
    return library_->path();
}

int32_t LegacyEffectHandle::Command(uint32_t cmd, uint32_t cmd_size, void* cmd_data,
                                    uint32_t* reply_size, void* reply_data) {
    if (handle_ == nullptr || *handle_ == nullptr || (*handle_)->command == nullptr) {
        return -EINVAL;
    }
    const int32_t status =
            (*handle_)->command(handle_, cmd, cmd_size, cmd_data, reply_size, reply_data);
    if (status != 0) {
        LOG(DEBUG) << descriptor_.name << ": command " << cmd << " returned " << status;
    }
    return status;
}

int32_t LegacyEffectHandle::CommandWithStatusReply(uint32_t cmd, uint32_t cmd_size,
                                                   void* cmd_data) {
    int32_t reply = 0;
    uint32_t reply_size = sizeof(reply);
    const int32_t status = Command(cmd, cmd_size, cmd_data, &reply_size, &reply);
    if (status != 0) return status;
    // Legacy effects report the outcome of many commands in the reply
    // rather than in the return value.
    if (reply_size == sizeof(reply) && reply != 0) {
        LOG(DEBUG) << descriptor_.name << ": command " << cmd << " replied " << reply;
        return reply;
    }
    return 0;
}

int32_t LegacyEffectHandle::Process(audio_buffer_t* in, audio_buffer_t* out) {
    if (handle_ == nullptr || *handle_ == nullptr || (*handle_)->process == nullptr) {
        return -EINVAL;
    }
    return (*handle_)->process(handle_, in, out);
}

// --- LegacyLibrary -----------------------------------------------------------

LegacyLibrary::LegacyLibrary(std::string name, std::string path, void* dl_handle,
                             const audio_effect_library_t* info)
    : name_(std::move(name)), path_(std::move(path)), dl_handle_(dl_handle), info_(info) {}

LegacyLibrary::~LegacyLibrary() {
    // Legacy libraries are not reliably unloadable (static state, threads).
    // Keep them mapped for the lifetime of the process like the legacy
    // EffectsFactory did.
    LOG(DEBUG) << __func__ << ": " << path_ << " (kept mapped)";
}

std::shared_ptr<LegacyLibrary> LegacyLibrary::Open(const std::string& name,
                                                   const std::string& path) {
    void* dl_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (dl_handle == nullptr) {
        LOG(ERROR) << __func__ << ": dlopen(" << path << ") failed: " << dlerror();
        return nullptr;
    }
    auto* info = static_cast<const audio_effect_library_t*>(
            dlsym(dl_handle, AUDIO_EFFECT_LIBRARY_INFO_SYM_AS_STR));
    if (info == nullptr) {
        LOG(ERROR) << __func__ << ": " << path << " has no " << AUDIO_EFFECT_LIBRARY_INFO_SYM_AS_STR
                   << " symbol: " << dlerror();
        dlclose(dl_handle);
        return nullptr;
    }
    if (info->tag != AUDIO_EFFECT_LIBRARY_TAG) {
        LOG(ERROR) << __func__ << ": " << path << " has a bad library tag 0x" << std::hex
                   << info->tag;
        dlclose(dl_handle);
        return nullptr;
    }
    if (EFFECT_API_VERSION_MAJOR(info->version) !=
        EFFECT_API_VERSION_MAJOR(EFFECT_LIBRARY_API_VERSION)) {
        LOG(ERROR) << __func__ << ": " << path << " implements library API version "
                   << EFFECT_API_VERSION_MAJOR(info->version) << "."
                   << ApiVersionMinor(info->version) << ", expected major "
                   << EFFECT_API_VERSION_MAJOR(EFFECT_LIBRARY_API_VERSION);
        dlclose(dl_handle);
        return nullptr;
    }
    if (info->create_effect == nullptr || info->release_effect == nullptr ||
        info->get_descriptor == nullptr) {
        LOG(ERROR) << __func__ << ": " << path << " has an incomplete library interface";
        dlclose(dl_handle);
        return nullptr;
    }
    LOG(INFO) << __func__ << ": loaded \"" << name << "\" from " << path << ": "
              << (info->name != nullptr ? info->name : "?") << " by "
              << (info->implementor != nullptr ? info->implementor : "?") << ", API "
              << EFFECT_API_VERSION_MAJOR(info->version) << "." << ApiVersionMinor(info->version);
    return std::shared_ptr<LegacyLibrary>(new LegacyLibrary(name, path, dl_handle, info));
}

std::optional<effect_descriptor_t> LegacyLibrary::GetDescriptor(const effect_uuid_t& uuid) {
    effect_descriptor_t descriptor = {};
    if (const int32_t status = info_->get_descriptor(&uuid, &descriptor); status != 0) {
        LOG(WARNING) << __func__ << ": " << name_ << " has no effect " << UuidToString(uuid) << " ("
                     << status << ")";
        return std::nullopt;
    }
    return descriptor;
}

std::unique_ptr<LegacyEffectHandle> LegacyLibrary::CreateEffect(const effect_uuid_t& uuid,
                                                                int32_t session_id, int32_t io_id) {
    effect_handle_t handle = nullptr;
    const int32_t status = info_->create_effect(&uuid, session_id, io_id, &handle);
    if (status != 0 || handle == nullptr) {
        LOG(ERROR) << __func__ << ": " << name_ << " failed to create " << UuidToString(uuid)
                   << " (session " << session_id << ", io " << io_id << "): " << status;
        return nullptr;
    }
    effect_descriptor_t descriptor = {};
    if (*handle != nullptr && (*handle)->get_descriptor != nullptr) {
        (*handle)->get_descriptor(handle, &descriptor);
    }
    if (descriptor.name[0] == '\0') {
        if (auto lib_descriptor = GetDescriptor(uuid); lib_descriptor.has_value()) {
            descriptor = *lib_descriptor;
        }
    }
    LOG(DEBUG) << __func__ << ": " << name_ << " created \"" << descriptor.name << "\" ("
               << UuidToString(uuid) << ") session " << session_id << " io " << io_id;
    return std::unique_ptr<LegacyEffectHandle>(
            new LegacyEffectHandle(shared_from_this(), handle, descriptor));
}

void LegacyLibrary::ReleaseEffect(effect_handle_t handle) {
    if (const int32_t status = info_->release_effect(handle); status != 0) {
        LOG(WARNING) << __func__ << ": " << name_ << " release_effect failed: " << status;
    }
}

}  // namespace aidl::android::hardware::audio::effect::legacy
