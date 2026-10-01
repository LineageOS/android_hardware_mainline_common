/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Device"

#include "device/CameraDevice.h"

#include <android-base/logging.h>
#include <system/camera_metadata.h>

#include "device/RequestTemplates.h"
#include "utils/Status.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::aidl::android::hardware::camera::common::Status;

// Relative cost of a camera for the framework's resource arbitration. A
// plain capture device is independent of the others, so several of them can
// be open, bandwidth permitting. Sensors behind a media controller share its
// pipeline stages, so only one of them can be open at a time.
constexpr int32_t kResourceCost = 50;
constexpr int32_t kPipelineResourceCost = 100;

}  // namespace

CameraDevice::CameraDevice(std::string name, std::shared_ptr<const CameraDescription> description,
                           std::shared_ptr<Flash> flash, DeviceOpeners open,
                           std::function<std::shared_ptr<GraphicBuffers>()> buffers)
    : name_(std::move(name)),
      description_(std::move(description)),
      flash_(std::move(flash)),
      open_(std::move(open)),
      buffers_(std::move(buffers)) {}

void CameraDevice::Disconnect() {
    disconnected_ = true;
    if (flash_ != nullptr) flash_->Detach();
}

::ndk::ScopedAStatus CameraDevice::getCameraCharacteristics(
        device::CameraMetadata* characteristics) {
    *characteristics = description_->characteristics().ToAidl();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDevice::getPhysicalCameraCharacteristics(
        const std::string& /*physical_id*/, device::CameraMetadata* characteristics) {
    // Not a logical multi-camera.
    *characteristics = {};
    return ToBinderStatus(Status::ILLEGAL_ARGUMENT);
}

::ndk::ScopedAStatus CameraDevice::getResourceCost(
        ::aidl::android::hardware::camera::common::CameraResourceCost* cost) {
    cost->resourceCost =
            description_->candidate().pipeline != nullptr ? kPipelineResourceCost : kResourceCost;
    cost->conflictingDevices.clear();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDevice::isStreamCombinationSupported(
        const device::StreamConfiguration& streams, bool* supported) {
    std::string why;
    *supported = description_->PlanStreams(streams, &why).has_value();
    if (!*supported) LOG(DEBUG) << name_ << ": stream combination not supported: " << why;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDevice::open(
        const std::shared_ptr<device::ICameraDeviceCallback>& callback,
        std::shared_ptr<device::ICameraDeviceSession>* session) {
    *session = nullptr;
    if (callback == nullptr) return ToBinderStatus(Status::ILLEGAL_ARGUMENT);
    if (disconnected_) return ToBinderStatus(Status::CAMERA_DISCONNECTED);

    std::lock_guard<std::mutex> lock(session_lock_);
    if (auto current = session_.lock(); current != nullptr && !current->IsClosed()) {
        LOG(ERROR) << name_ << ": already open";
        return ToBinderStatus(Status::CAMERA_IN_USE);
    }
    // The torch goes off and is unavailable while the camera is open.
    if (flash_ != nullptr) flash_->Acquire();
    Status status;
    auto opened = CameraDeviceSession::Create(name_, description_, callback, open_, buffers_(),
                                              &status, flash_);
    if (opened == nullptr) {
        if (flash_ != nullptr) flash_->Release();
        return ToBinderStatus(status);
    }
    session_ = opened;
    *session = opened;
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDevice::openInjectionSession(
        const std::shared_ptr<device::ICameraDeviceCallback>& /*callback*/,
        std::shared_ptr<device::ICameraInjectionSession>* session) {
    *session = nullptr;
    return ToBinderStatus(Status::OPERATION_NOT_SUPPORTED);
}

::ndk::ScopedAStatus CameraDevice::setTorchMode(bool on) {
    if (flash_ == nullptr) return ToBinderStatus(Status::OPERATION_NOT_SUPPORTED);
    if (disconnected_) return ToBinderStatus(Status::CAMERA_DISCONNECTED);
    return ToBinderStatus(flash_->SetTorch(on));
}

::ndk::ScopedAStatus CameraDevice::turnOnTorchWithStrengthLevel(int32_t level) {
    if (flash_ == nullptr) return ToBinderStatus(Status::OPERATION_NOT_SUPPORTED);
    if (disconnected_) return ToBinderStatus(Status::CAMERA_DISCONNECTED);
    return ToBinderStatus(flash_->SetTorchLevel(level));
}

::ndk::ScopedAStatus CameraDevice::getTorchStrengthLevel(int32_t* level) {
    if (flash_ == nullptr) return ToBinderStatus(Status::OPERATION_NOT_SUPPORTED);
    *level = flash_->torch_level();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDevice::constructDefaultRequestSettings(
        device::RequestTemplate type, device::CameraMetadata* settings) {
    auto metadata = BuildRequestTemplate(*description_, type);
    if (!metadata.has_value()) {
        *settings = {};
        return ToBinderStatus(Status::ILLEGAL_ARGUMENT);
    }
    *settings = metadata->ToAidl();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDevice::isStreamCombinationWithSettingsSupported(
        const device::StreamConfiguration& streams, bool* supported) {
    // No setting (session parameter) changes what the camera can do.
    return isStreamCombinationSupported(streams, supported);
}

::ndk::ScopedAStatus CameraDevice::getSessionCharacteristics(
        const device::StreamConfiguration& config, device::CameraMetadata* characteristics) {
    std::string why;
    if (!description_->PlanStreams(config, &why).has_value()) {
        LOG(DEBUG) << name_ << ": getSessionCharacteristics: not supported: " << why;
        *characteristics = {};
        return ToBinderStatus(Status::ILLEGAL_ARGUMENT);
    }
    // Zoom does not depend on the streams.
    Metadata session;
    session.SetFloat(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, description_->max_zoom());
    session.Set(ANDROID_CONTROL_ZOOM_RATIO_RANGE,
                std::vector<float>{1.0f, description_->max_zoom()});
    *characteristics = session.ToAidl();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus CameraDevice::warmUp() {
    return ::ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::camera::mainline
