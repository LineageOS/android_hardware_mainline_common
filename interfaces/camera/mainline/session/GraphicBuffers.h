/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>

#include <aidl/android/hardware/common/NativeHandle.h>
#include <cutils/native_handle.h>

#include "convert/Image.h"

namespace aidl::android::hardware::camera::mainline {

// Access to the graphic buffers the framework hands out for the output
// streams. Abstract so that sessions can be tested without gralloc.
class GraphicBuffers {
  public:
    virtual ~GraphicBuffers() = default;

    // Imports a buffer received over binder. Returns nullptr on failure.
    virtual buffer_handle_t Import(
            const ::aidl::android::hardware::common::NativeHandle& handle) = 0;
    virtual void Free(buffer_handle_t buffer) = 0;

    // Locks a YUV 4:2:0 buffer of `size` for CPU writes.
    virtual std::optional<YuvDestination> LockYuv(buffer_handle_t buffer, Size size) = 0;
    // Locks an RGBA buffer, or a BLOB buffer (whose size is {bytes, 1}), for
    // CPU writes.
    virtual std::optional<RgbaDestination> Lock(buffer_handle_t buffer, Size size) = 0;
    virtual void Unlock(buffer_handle_t buffer) = 0;
};

// The real thing, backed by the device's gralloc mapper.
std::shared_ptr<GraphicBuffers> CreateGrallocBuffers();

}  // namespace aidl::android::hardware::camera::mainline
