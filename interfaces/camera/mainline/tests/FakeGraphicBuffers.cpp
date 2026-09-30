/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tests/FakeGraphicBuffers.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

int IdOf(buffer_handle_t buffer) {
    return buffer->data[buffer->numFds];
}

}  // namespace

FakeGraphicBuffers::~FakeGraphicBuffers() = default;

::aidl::android::hardware::common::NativeHandle FakeGraphicBuffers::Handle(int id) {
    ::aidl::android::hardware::common::NativeHandle handle;
    handle.ints.push_back(id);
    return handle;
}

buffer_handle_t FakeGraphicBuffers::Import(
        const ::aidl::android::hardware::common::NativeHandle& handle) {
    if (handle.ints.empty()) return nullptr;
    native_handle_t* imported = native_handle_create(0, 1);
    imported->data[0] = handle.ints[0];
    std::lock_guard<std::mutex> lock(lock_);
    ++imported_;
    return imported;
}

void FakeGraphicBuffers::Free(buffer_handle_t buffer) {
    native_handle_delete(const_cast<native_handle_t*>(buffer));
    std::lock_guard<std::mutex> lock(lock_);
    ++freed_;
}

std::optional<YuvDestination> FakeGraphicBuffers::LockYuv(buffer_handle_t buffer, Size size) {
    std::lock_guard<std::mutex> lock(lock_);
    const size_t luma = static_cast<size_t>(size.width) * size.height;
    const size_t chroma = static_cast<size_t>((size.width + 1) / 2) * ((size.height + 1) / 2);
    auto& memory = memory_[IdOf(buffer)];
    memory.assign(luma + 2 * chroma, 0xee);
    ++locked_;
    return YuvDestination{.y = memory.data(),
                          .cb = memory.data() + luma,
                          .cr = memory.data() + luma + chroma,
                          .y_stride = size.width,
                          .c_stride = (size.width + 1) / 2,
                          .c_step = 1};
}

std::optional<RgbaDestination> FakeGraphicBuffers::Lock(buffer_handle_t buffer, Size size) {
    std::lock_guard<std::mutex> lock(lock_);
    auto& memory = memory_[IdOf(buffer)];
    memory.assign(static_cast<size_t>(size.width) * size.height * 4, 0xee);
    ++locked_;
    return RgbaDestination{.data = memory.data(), .stride_bytes = size.width * 4};
}

void FakeGraphicBuffers::Unlock(buffer_handle_t /*buffer*/) {}

std::vector<uint8_t> FakeGraphicBuffers::Contents(int id) {
    std::lock_guard<std::mutex> lock(lock_);
    return memory_[id];
}

}  // namespace aidl::android::hardware::camera::mainline
