/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <libsensors_mainline/SensorBackend.h>

#include <memory>
#include <string>
#include <vector>

namespace aidl::android::hardware::sensors::mainline {

// A backend library loaded with dlopen().
struct LoadedBackend {
    std::string library;  // as requested, e.g. "libsensors_iio.so"
    std::string path;     // path that was actually opened
    std::unique_ptr<ISensorBackend> backend;
    uint32_t flags = 0;
    void* dl_handle = nullptr;

    LoadedBackend() = default;
    LoadedBackend(LoadedBackend&& other) noexcept;
    LoadedBackend& operator=(LoadedBackend&& other) noexcept;
    LoadedBackend(const LoadedBackend&) = delete;
    LoadedBackend& operator=(const LoadedBackend&) = delete;
    // Destroys the backend instance and closes the library.
    ~LoadedBackend();
};

/*
 * Resolves the list of backend libraries to load and loads them.
 *
 * The list comes from, in order of precedence:
 *  1. the "backends" setting (property vendor.sensors.backends or config file),
 *  2. the build time default (LOAD_CUSTOM_BACKENDS soong config variable),
 *  3. the built-in list: iio, input, mock.
 * Entries are short names ("iio"), library names ("libsensors_iio.so") or
 * absolute paths.
 *
 * Libraries are searched in the linker namespace of the HAL (which includes
 * the APEX), then in /odm and /vendor "lib{,64}/hw" and "lib{,64}" so that
 * out-of-tree backends can be shipped on /vendor (this requires the APEX
 * linker configuration to permit /vendor/${LIB}).
 */
class BackendLoader {
  public:
    static std::vector<std::string> ResolveBackendList();
    static std::vector<LoadedBackend> LoadBackends(const std::vector<std::string>& libraries);
    static std::unique_ptr<LoadedBackend> LoadBackend(const std::string& library);

  private:
    static std::string ExpandName(const std::string& name);
};

}  // namespace aidl::android::hardware::sensors::mainline
