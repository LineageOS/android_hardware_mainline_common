/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace aidl::android::hardware::audio::core::mainline {

// Stand-in for a sound card that does not exist: playback data is discarded,
// capture data is silence. Transfers are paced in real time so that the
// framework sees a device that behaves like real hardware.
class NullDevice {
  public:
    explicit NullDevice(int sample_rate) : sample_rate_(sample_rate) {}

    // Resets the pacing clock. Call whenever the stream (re)starts.
    void Start();
    // Blocks for the duration of `frames` (minus the time already elapsed) and,
    // for input, fills the buffer with silence.
    void Transfer(void* buffer, size_t frames, size_t frame_size_bytes, bool is_input);
    // Playback latency to report: one transfer worth of frames.
    int32_t LatencyMs(size_t frames) const;

  private:
    const int sample_rate_;
    int64_t start_time_ns_ = 0;
    int64_t frames_since_start_ = 0;
};

}  // namespace aidl::android::hardware::audio::core::mainline
