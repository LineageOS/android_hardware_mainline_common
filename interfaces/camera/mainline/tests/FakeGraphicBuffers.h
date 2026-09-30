/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <mutex>
#include <vector>

#include "session/GraphicBuffers.h"

namespace aidl::android::hardware::camera::mainline {

// GraphicBuffers on plain memory. A buffer is identified by the first int of
// its NativeHandle; YUV buffers are planar I420 without padding.
class FakeGraphicBuffers : public GraphicBuffers {
  public:
    ~FakeGraphicBuffers() override;

    buffer_handle_t Import(const ::aidl::android::hardware::common::NativeHandle& handle) override;
    void Free(buffer_handle_t buffer) override;
    std::optional<YuvDestination> LockYuv(buffer_handle_t buffer, Size size) override;
    std::optional<RgbaDestination> Lock(buffer_handle_t buffer, Size size) override;
    void Unlock(buffer_handle_t buffer) override;

    // Contents of the buffer with `id`.
    std::vector<uint8_t> Contents(int id);
    int imported() const { return imported_; }
    int freed() const { return freed_; }
    int locked() const { return locked_; }

    static ::aidl::android::hardware::common::NativeHandle Handle(int id);

  private:
    std::mutex lock_;
    std::map<int, std::vector<uint8_t>> memory_;
    int imported_ = 0;
    int freed_ = 0;
    int locked_ = 0;
};

}  // namespace aidl::android::hardware::camera::mainline
