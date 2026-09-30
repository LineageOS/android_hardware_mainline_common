/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "passthrough/Format.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// Packs an encoded stream into IEC 61937 data bursts. A thin adapter over the
// libaudiospdif fork in ../spdif, the only user of it: syncing or replacing
// the fork only touches this class.
class Encoder {
  public:
    // Receives the bursts, as bytes of 16-bit little endian PCM frames.
    using Output = std::function<void(const uint8_t* data, size_t bytes)>;

    // nullptr when the fork can not pack `format` (e.g. kIec61937, which needs
    // no packing).
    static std::unique_ptr<Encoder> Create(EncodedFormat format, Output output);
    ~Encoder();

    // Takes encoded data; frames do not have to be aligned. Complete bursts
    // go to the output before this returns.
    void Write(const void* data, size_t bytes);
    // Drops buffered data and waits for the next frame start (flush, seek).
    void Reset();
    // Channels the bursts are sent on.
    unsigned int OutputChannels() const;

  private:
    class Impl;
    explicit Encoder(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
