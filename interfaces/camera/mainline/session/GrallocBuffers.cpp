/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_Gralloc"

#include <aidl/android/hardware/graphics/common/BufferUsage.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <android-base/logging.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include "session/GraphicBuffers.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::aidl::android::hardware::graphics::common::BufferUsage;
using ::android::GraphicBufferMapper;

constexpr uint32_t kWriteUsage = static_cast<uint32_t>(BufferUsage::CPU_WRITE_OFTEN);

class GrallocBuffers : public GraphicBuffers {
  public:
    buffer_handle_t Import(const ::aidl::android::hardware::common::NativeHandle& handle) override {
        // Borrows the file descriptors of `handle`; importing clones them.
        native_handle_t* raw = ::android::makeFromAidl(handle);
        if (raw == nullptr) return nullptr;
        buffer_handle_t imported = nullptr;
        const auto status = GraphicBufferMapper::get().importBufferNoValidate(raw, &imported);
        native_handle_delete(raw);
        if (status != ::android::OK) {
            LOG(ERROR) << "failed to import a buffer: " << status;
            return nullptr;
        }
        return imported;
    }

    void Free(buffer_handle_t buffer) override { GraphicBufferMapper::get().freeBuffer(buffer); }

    std::optional<YuvDestination> LockYuv(buffer_handle_t buffer, Size size) override {
        android_ycbcr ycbcr = {};
        const auto status = GraphicBufferMapper::get().lockYCbCr(
                buffer, kWriteUsage, ::android::Rect(size.width, size.height), &ycbcr);
        if (status != ::android::OK) {
            LOG(ERROR) << "failed to lock a YUV buffer: " << status;
            return std::nullopt;
        }
        return YuvDestination{
                .y = static_cast<uint8_t*>(ycbcr.y),
                .cb = static_cast<uint8_t*>(ycbcr.cb),
                .cr = static_cast<uint8_t*>(ycbcr.cr),
                .y_stride = static_cast<int32_t>(ycbcr.ystride),
                .c_stride = static_cast<int32_t>(ycbcr.cstride),
                .c_step = static_cast<int32_t>(ycbcr.chroma_step),
        };
    }

    std::optional<RgbaDestination> Lock(buffer_handle_t buffer, Size size) override {
        void* address = nullptr;
        const auto status = GraphicBufferMapper::get().lock(
                buffer, kWriteUsage, ::android::Rect(size.width, size.height), &address);
        if (status != ::android::OK || address == nullptr) {
            LOG(ERROR) << "failed to lock a buffer: " << status;
            return std::nullopt;
        }
        int32_t stride = size.width * 4;
        std::vector<::android::ui::PlaneLayout> layouts;
        if (GraphicBufferMapper::get().getPlaneLayouts(buffer, &layouts) == ::android::OK &&
            !layouts.empty() && layouts[0].strideInBytes > 0) {
            stride = static_cast<int32_t>(layouts[0].strideInBytes);
        }
        return RgbaDestination{.data = static_cast<uint8_t*>(address), .stride_bytes = stride};
    }

    void Unlock(buffer_handle_t buffer) override {
        if (GraphicBufferMapper::get().unlock(buffer) != ::android::OK) {
            LOG(WARNING) << "failed to unlock a buffer";
        }
    }
};

}  // namespace

std::shared_ptr<GraphicBuffers> CreateGrallocBuffers() {
    return std::make_shared<GrallocBuffers>();
}

}  // namespace aidl::android::hardware::camera::mainline
