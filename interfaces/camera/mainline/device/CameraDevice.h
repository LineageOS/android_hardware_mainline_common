/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <aidl/android/hardware/camera/device/BnCameraDevice.h>

#include "device/CameraDescription.h"
#include "session/CameraDeviceSession.h"
#include "session/GraphicBuffers.h"

namespace aidl::android::hardware::camera::mainline {

class CameraDevice : public device::BnCameraDevice {
  public:
    CameraDevice(std::string name, std::shared_ptr<const CameraDescription> description,
                 VideoDeviceOpener open = OpenVideoDevice,
                 std::function<std::shared_ptr<GraphicBuffers>()> buffers = CreateGrallocBuffers);

    // The device went away. Further calls fail with CAMERA_DISCONNECTED.
    void Disconnect();

    // ICameraDevice
    ::ndk::ScopedAStatus getCameraCharacteristics(device::CameraMetadata* characteristics) override;
    ::ndk::ScopedAStatus getPhysicalCameraCharacteristics(
            const std::string& physical_id, device::CameraMetadata* characteristics) override;
    ::ndk::ScopedAStatus getResourceCost(
            ::aidl::android::hardware::camera::common::CameraResourceCost* cost) override;
    ::ndk::ScopedAStatus isStreamCombinationSupported(const device::StreamConfiguration& streams,
                                                      bool* supported) override;
    ::ndk::ScopedAStatus open(const std::shared_ptr<device::ICameraDeviceCallback>& callback,
                              std::shared_ptr<device::ICameraDeviceSession>* session) override;
    ::ndk::ScopedAStatus openInjectionSession(
            const std::shared_ptr<device::ICameraDeviceCallback>& callback,
            std::shared_ptr<device::ICameraInjectionSession>* session) override;
    ::ndk::ScopedAStatus setTorchMode(bool on) override;
    ::ndk::ScopedAStatus turnOnTorchWithStrengthLevel(int32_t level) override;
    ::ndk::ScopedAStatus getTorchStrengthLevel(int32_t* level) override;
    ::ndk::ScopedAStatus constructDefaultRequestSettings(device::RequestTemplate type,
                                                         device::CameraMetadata* settings) override;
    ::ndk::ScopedAStatus isStreamCombinationWithSettingsSupported(
            const device::StreamConfiguration& streams, bool* supported) override;
    ::ndk::ScopedAStatus getSessionCharacteristics(
            const device::StreamConfiguration& config,
            device::CameraMetadata* characteristics) override;
    ::ndk::ScopedAStatus warmUp() override;

  private:
    const std::string name_;
    const std::shared_ptr<const CameraDescription> description_;
    const VideoDeviceOpener open_;
    const std::function<std::shared_ptr<GraphicBuffers>()> buffers_;
    std::atomic<bool> disconnected_ = false;

    // The open session, if any: a camera has one session at a time.
    std::mutex session_lock_;
    std::weak_ptr<CameraDeviceSession> session_;
};

}  // namespace aidl::android::hardware::camera::mainline
