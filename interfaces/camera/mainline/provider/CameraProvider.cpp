/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Provider"

#include "provider/CameraProvider.h"

#include <algorithm>

#include <android-base/logging.h>

#include "utils/Status.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::aidl::android::hardware::camera::common::CameraDeviceStatus;
using ::aidl::android::hardware::camera::common::Status;

constexpr char kDeviceNamePrefix[] = "device@1.1/internal/";

}  // namespace

CameraProvider::CameraProvider(const Properties& properties)
    : properties_(properties), hwdb_(CameraHwdb::Load()), ids_(properties.external_id_offset) {}

CameraProvider::~CameraProvider() {
    monitor_.reset();
}

void CameraProvider::Start() {
    Rescan();
    monitor_ = std::make_unique<DeviceMonitor>("/dev", [this] { return Rescan(); });
    monitor_->Start();
}

bool CameraProvider::WaitForInternalCameras(int count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(lock_);
    LOG(INFO) << "waiting up to " << timeout.count() << " ms for " << count
              << " internal camera(s), " << InternalCountLocked() << " present";
    const bool ok =
            changed_.wait_for(lock, timeout, [&] { return InternalCountLocked() >= count; });
    if (!ok) {
        LOG(WARNING) << "only " << InternalCountLocked() << " of " << count
                     << " internal camera(s) appeared in time";
    }
    return ok;
}

int CameraProvider::InternalCountLocked() const {
    return static_cast<int>(std::count_if(cameras_.begin(), cameras_.end(), [](const auto& entry) {
        return entry.second.candidate.internal;
    }));
}

bool CameraProvider::Rescan() {
    std::vector<StatusChange> changes;
    bool retry;
    {
        std::lock_guard<std::mutex> lock(lock_);

        std::vector<CameraCandidate> known;
        for (const auto& [key, camera] : cameras_) known.push_back(camera.candidate);
        DiscoveryResult result = DiscoverCameras(properties_, hwdb_.get(), known);
        retry = result.retry;

        // Removed cameras. A camera whose capture node changed (e.g. replugged
        // while the old node was still open) is removed and added again; it
        // keeps its ID.
        for (auto it = cameras_.begin(); it != cameras_.end();) {
            const bool present =
                    std::any_of(result.cameras.begin(), result.cameras.end(),
                                [&](const CameraCandidate& candidate) {
                                    return candidate.key == it->first &&
                                           candidate.info.path == it->second.candidate.info.path;
                                });
            if (present) {
                ++it;
                continue;
            }
            LOG(INFO) << "camera " << it->second.name << " (" << it->first << ") removed";
            changes.push_back({it->second.name, CameraDeviceStatus::NOT_PRESENT});
            it->second.device->Disconnect();
            ids_.Release(it->first);
            it = cameras_.erase(it);
        }

        // New cameras, internal ones first (back before front, as apps
        // expect camera 0 to face back) and in a stable order so that their
        // IDs do not depend on the probe order.
        std::vector<CameraCandidate> added;
        for (auto& candidate : result.cameras) {
            if (cameras_.count(candidate.key) == 0) added.push_back(std::move(candidate));
        }
        std::sort(added.begin(), added.end(),
                  [](const CameraCandidate& a, const CameraCandidate& b) {
                      if (a.internal != b.internal) return a.internal;
                      if (a.internal && a.facing != b.facing) return a.facing == Facing::kBack;
                      return a.key < b.key;
                  });
        for (auto& candidate : added) {
            auto description = CameraDescription::Create(candidate);
            if (description == nullptr) {
                LOG(ERROR) << "camera " << candidate.key << " is not usable, skipped";
                continue;
            }
            Camera camera;
            camera.id = ids_.Allocate(candidate.key, candidate.internal);
            camera.name = kDeviceNamePrefix + std::to_string(camera.id);
            camera.device = ndk::SharedRefBase::make<CameraDevice>(camera.name, description);
            camera.candidate = std::move(candidate);
            LOG(INFO) << "camera " << camera.name << " (" << camera.candidate.key << ") added, "
                      << (camera.candidate.internal ? "internal" : "external");
            changes.push_back({camera.name, CameraDeviceStatus::PRESENT});
            cameras_.emplace(camera.candidate.key, std::move(camera));
        }
    }
    changed_.notify_all();
    Notify(changes);
    return retry;
}

void CameraProvider::Notify(const std::vector<StatusChange>& changes) {
    std::lock_guard<std::mutex> lock(callback_lock_);
    if (callback_ == nullptr) return;
    for (const auto& change : changes) {
        const auto status = callback_->cameraDeviceStatusChange(change.name, change.status);
        if (!status.isOk()) {
            LOG(WARNING) << "cameraDeviceStatusChange(" << change.name
                         << "): " << status.getDescription();
        }
    }
}

::ndk::ScopedAStatus CameraProvider::setCallback(
        const std::shared_ptr<provider::ICameraProviderCallback>& callback) {
    if (callback == nullptr) return ToBinderStatus(Status::ILLEGAL_ARGUMENT);

    std::lock_guard<std::mutex> callback_lock(callback_lock_);
    callback_ = callback;

    // External cameras are only known to the framework through the callback.
    // Internal ones are reported by getCameraIdList().
    std::vector<std::string> external;
    {
        std::lock_guard<std::mutex> lock(lock_);
        for (const auto& [key, camera] : cameras_) {
            if (!camera.candidate.internal) external.push_back(camera.name);
        }
    }
    for (const auto& name : external) {
        callback_->cameraDeviceStatusChange(name, CameraDeviceStatus::PRESENT);
    }
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraProvider::getVendorTags(
        std::vector<::aidl::android::hardware::camera::common::VendorTagSection>* tags) {
    tags->clear();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraProvider::getCameraIdList(std::vector<std::string>* ids) {
    std::lock_guard<std::mutex> lock(lock_);
    std::vector<std::pair<int, std::string>> internal;
    for (const auto& [key, camera] : cameras_) {
        if (camera.candidate.internal) internal.emplace_back(camera.id, camera.name);
    }
    std::sort(internal.begin(), internal.end());
    ids->clear();
    for (auto& [id, name] : internal) ids->push_back(std::move(name));
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraProvider::getCameraDeviceInterface(
        const std::string& name, std::shared_ptr<device::ICameraDevice>* device) {
    *device = nullptr;
    std::lock_guard<std::mutex> lock(lock_);
    for (const auto& [key, camera] : cameras_) {
        if (camera.name == name) {
            *device = camera.device;
            return ::ndk::ScopedAStatus::ok();
        }
    }
    LOG(WARNING) << "getCameraDeviceInterface: unknown camera " << name;
    return ToBinderStatus(Status::ILLEGAL_ARGUMENT);
}

::ndk::ScopedAStatus CameraProvider::notifyDeviceStateChange(int64_t /*state*/) {
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraProvider::getConcurrentCameraIds(
        std::vector<provider::ConcurrentCameraIdCombination>* combinations) {
    // Every camera is an independent device, but whether two of them can
    // stream at the same time depends on the bus bandwidth, which is not
    // known up front. Claim nothing.
    combinations->clear();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraProvider::isConcurrentStreamCombinationSupported(
        const std::vector<provider::CameraIdAndStreamCombination>& /*configs*/, bool* supported) {
    *supported = false;
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::camera::mainline
