/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <aidl/android/hardware/camera/provider/BnCameraProvider.h>

#include "Properties.h"
#include "provider/CameraIdAllocator.h"
#include "provider/DeviceMonitor.h"
#include "provider/Discovery.h"

namespace aidl::android::hardware::camera::mainline {

class CameraProvider : public provider::BnCameraProvider {
  public:
    explicit CameraProvider(const Properties& properties);
    ~CameraProvider() override;

    // Scans for cameras and starts watching for hotplug.
    void Start();

    // Blocks until `count` internal cameras are present or `timeout` passed.
    // Returns whether they are.
    bool WaitForInternalCameras(int count, std::chrono::milliseconds timeout);

    // ICameraProvider
    ::ndk::ScopedAStatus setCallback(
            const std::shared_ptr<provider::ICameraProviderCallback>& callback) override;
    ::ndk::ScopedAStatus getVendorTags(std::vector<common::VendorTagSection>* tags) override;
    ::ndk::ScopedAStatus getCameraIdList(std::vector<std::string>* ids) override;
    ::ndk::ScopedAStatus getCameraDeviceInterface(
            const std::string& name, std::shared_ptr<device::ICameraDevice>* device) override;
    ::ndk::ScopedAStatus notifyDeviceStateChange(int64_t state) override;
    ::ndk::ScopedAStatus getConcurrentCameraIds(
            std::vector<provider::ConcurrentCameraIdCombination>* combinations) override;
    ::ndk::ScopedAStatus isConcurrentStreamCombinationSupported(
            const std::vector<provider::CameraIdAndStreamCombination>& configs,
            bool* supported) override;

  private:
    struct Camera {
        CameraCandidate candidate;
        int id = -1;
        // "device@1.1/internal/<id>"
        std::string name;
    };

    // A status change to send to the framework.
    struct StatusChange {
        std::string name;
        common::CameraDeviceStatus status;
    };

    // Runs discovery and updates the camera list. Returns whether discovery
    // wants to run again later.
    bool Rescan();
    void Notify(const std::vector<StatusChange>& changes);
    int InternalCountLocked() const;

    const Properties properties_;

    std::mutex lock_;
    std::condition_variable changed_;
    // By CameraCandidate::key.
    std::map<std::string, Camera> cameras_;
    CameraIdAllocator ids_;

    // Serializes calls into the framework, so that status changes arrive in
    // order. lock_ may be taken while holding it, never the other way round.
    std::mutex callback_lock_;
    std::shared_ptr<provider::ICameraProviderCallback> callback_;

    // Destroyed first, so that its thread no longer runs Rescan().
    std::unique_ptr<DeviceMonitor> monitor_;
};

}  // namespace aidl::android::hardware::camera::mainline
