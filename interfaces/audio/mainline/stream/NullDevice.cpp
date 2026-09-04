/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_NullDevice"

#include "stream/NullDevice.h"

#include <unistd.h>

#include <algorithm>
#include <cstring>

#include <android-base/logging.h>
#include <audio_utils/clock.h>

namespace aidl::android::hardware::audio::core::mainline {

void NullDevice::Start() {
    start_time_ns_ = ::android::uptimeNanos();
    frames_since_start_ = 0;
    LOG(DEBUG) << __func__;
}

void NullDevice::Transfer(void* buffer, size_t frames, size_t frame_size_bytes, bool is_input) {
    if (sample_rate_ <= 0) return;
    frames_since_start_ += static_cast<int64_t>(frames);
    const int64_t elapsed_us = (::android::uptimeNanos() - start_time_ns_) / NANOS_PER_MICROSECOND;
    const int64_t expected_us = frames_since_start_ * MICROS_PER_SECOND / sample_rate_;
    const int64_t burst_us = static_cast<int64_t>(frames) * MICROS_PER_SECOND / sample_rate_;
    const int64_t ahead_us = expected_us - elapsed_us;
    if (ahead_us > 0) {
        usleep(static_cast<useconds_t>(std::min(ahead_us, burst_us)));
    }
    if (is_input && buffer != nullptr) {
        std::memset(buffer, 0, frames * frame_size_bytes);
    }
}

int32_t NullDevice::LatencyMs(size_t frames) const {
    if (sample_rate_ <= 0) return 0;
    return static_cast<int32_t>(frames * 1000 / static_cast<size_t>(sample_rate_));
}

}  // namespace aidl::android::hardware::audio::core::mainline
