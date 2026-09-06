/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsLoader"

#include "BackendLoader.h"

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <libsensors_common/Settings.h>

#include <dlfcn.h>

#include <iomanip>

namespace aidl::android::hardware::sensors::mainline {

namespace {

#ifndef LOAD_CUSTOM_BACKENDS
#define LOAD_CUSTOM_BACKENDS ""
#endif

const char* const kDefaultBackends[] = {"iio", "input", "mock"};

const char* const kSearchDirs[] = {
#ifdef __LP64__
        "/odm/lib64/hw/",
        "/vendor/lib64/hw/",
        "/odm/lib64/",
        "/vendor/lib64/",
#else
        "/odm/lib/hw/",
        "/vendor/lib/hw/",
        "/odm/lib/",
        "/vendor/lib/",
#endif
};

}  // namespace

LoadedBackend::LoadedBackend(LoadedBackend&& other) noexcept
    : library(std::move(other.library)),
      path(std::move(other.path)),
      backend(std::move(other.backend)),
      flags(other.flags),
      dl_handle(other.dl_handle) {
    other.dl_handle = nullptr;
}

LoadedBackend& LoadedBackend::operator=(LoadedBackend&& other) noexcept {
    if (this != &other) {
        backend.reset();
        if (dl_handle != nullptr) {
            dlclose(dl_handle);
        }
        library = std::move(other.library);
        path = std::move(other.path);
        backend = std::move(other.backend);
        flags = other.flags;
        dl_handle = other.dl_handle;
        other.dl_handle = nullptr;
    }
    return *this;
}

LoadedBackend::~LoadedBackend() {
    // The backend object must be destroyed before its code is unmapped.
    backend.reset();
    if (dl_handle != nullptr) {
        dlclose(dl_handle);
        dl_handle = nullptr;
    }
}

std::string BackendLoader::ExpandName(const std::string& name) {
    if (name.find('/') != std::string::npos || ::android::base::EndsWith(name, ".so")) {
        return name;
    }
    if (::android::base::StartsWith(name, "libsensors_")) {
        return name + ".so";
    }
    return "libsensors_" + name + ".so";
}

std::vector<std::string> BackendLoader::ResolveBackendList() {
    std::string list = Settings::Get().GetString("backends", "");
    std::string source = "configuration";
    if (list.empty()) {
        list = LOAD_CUSTOM_BACKENDS;
        source = "build configuration";
    }

    std::vector<std::string> libraries;
    if (!list.empty()) {
        for (const auto& token : ::android::base::Split(list, ",")) {
            std::string name = ::android::base::Trim(token);
            if (!name.empty()) {
                libraries.push_back(ExpandName(name));
            }
        }
    }
    if (libraries.empty()) {
        source = "built-in default";
        for (const char* name : kDefaultBackends) {
            libraries.push_back(ExpandName(name));
        }
    }
    LOG(INFO) << "Backend list (" << source << "): " << ::android::base::Join(libraries, ", ");
    return libraries;
}

std::unique_ptr<LoadedBackend> BackendLoader::LoadBackend(const std::string& library) {
    std::vector<std::string> candidates;
    if (library.find('/') != std::string::npos) {
        candidates.push_back(library);
    } else {
        // Default namespace first (APEX bundled backends), then /odm and
        // /vendor.
        candidates.push_back(library);
        for (const char* dir : kSearchDirs) {
            candidates.push_back(std::string(dir) + library);
        }
    }

    void* handle = nullptr;
    std::string opened;
    for (const auto& candidate : candidates) {
        handle = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle != nullptr) {
            opened = candidate;
            break;
        }
        LOG(DEBUG) << "dlopen(" << candidate << ") failed: " << dlerror();
    }
    if (handle == nullptr) {
        LOG(WARNING) << "Backend library " << library << " not found";
        return nullptr;
    }

    auto loaded = std::make_unique<LoadedBackend>();
    loaded->library = library;
    loaded->path = opened;
    loaded->dl_handle = handle;

    auto version_func = reinterpret_cast<GetBackendInterfaceVersionFunc>(
            dlsym(handle, kBackendInterfaceVersionSymbol));
    uint32_t version = version_func != nullptr ? version_func() : 1;
    if (version != kSensorBackendInterfaceVersion) {
        LOG(ERROR) << "Backend " << opened << " was built against interface version " << version
                   << ", expected " << kSensorBackendInterfaceVersion << "; not loading it";
        return nullptr;
    }

    auto flags_func = reinterpret_cast<GetBackendFlagsFunc>(dlsym(handle, kBackendFlagsSymbol));
    loaded->flags = flags_func != nullptr ? flags_func() : 0;

    auto create_func = reinterpret_cast<CreateBackendFunc>(dlsym(handle, kCreateBackendSymbol));
    if (create_func == nullptr) {
        LOG(ERROR) << "Backend " << opened << " does not export " << kCreateBackendSymbol << ": "
                   << dlerror();
        return nullptr;
    }
    ISensorBackend* backend = create_func();
    if (backend == nullptr) {
        LOG(ERROR) << "Backend " << opened << " returned no instance";
        return nullptr;
    }
    loaded->backend.reset(backend);
    LOG(INFO) << "Loaded backend '" << backend->GetName() << "' from " << opened << " (interface v"
              << version << ", flags 0x" << std::hex << loaded->flags << std::dec << ")";
    return loaded;
}

std::vector<LoadedBackend> BackendLoader::LoadBackends(const std::vector<std::string>& libraries) {
    std::vector<LoadedBackend> backends;
    for (const auto& library : libraries) {
        auto loaded = LoadBackend(library);
        if (loaded) {
            backends.push_back(std::move(*loaded));
        }
    }
    return backends;
}

}  // namespace aidl::android::hardware::sensors::mainline
