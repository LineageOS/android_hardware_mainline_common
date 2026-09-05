/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <alsa/asoundlib.h>

#include "alsa/AlsaError.h"
#include "alsa/AlsaFormat.h"

namespace aidl::android::hardware::audio::core::mainline::alsa {

// Requested stream parameters.
struct PcmConfig {
    snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;
    unsigned int channels = 2;
    unsigned int rate = 48000;
    // Wanted period size; the driver may round it.
    snd_pcm_uframes_t period_frames = 0;
    // Wanted total ring buffer size; the driver may round it.
    snd_pcm_uframes_t buffer_frames = 0;

    std::string ToString() const;
};

// Position information for presentation / capture timestamps.
struct PcmPosition {
    // Frames still queued in the ring buffer (playback) or already captured
    // but not yet read (capture).
    snd_pcm_sframes_t delay_frames = 0;
    // CLOCK_MONOTONIC timestamp of the moment `delay_frames` was sampled.
    int64_t time_ns = 0;
};

// A thin RAII wrapper around an open snd_pcm_t. All methods must be called
// from a single thread (the stream worker thread).
class Pcm {
  public:
    // Opens `name` (e.g. "hw:0,0") and configures it. When the hardware does
    // not accept the exact configuration, the name is retried through the
    // alsa-lib "plug" layer which performs rate / format / channel conversion.
    static std::unique_ptr<Pcm> Open(const std::string& name, snd_pcm_stream_t stream,
                                     const PcmConfig& config);
    ~Pcm();

    Pcm(const Pcm&) = delete;
    Pcm& operator=(const Pcm&) = delete;

    const std::string& name() const { return name_; }
    snd_pcm_stream_t stream() const { return stream_; }
    // Effective configuration after hw_params negotiation.
    const PcmConfig& config() const { return config_; }
    bool is_plug() const { return is_plug_; }
    bool can_pause() const { return can_pause_; }

    // Playback: writes `frames` interleaved frames, blocking until done.
    // Capture: reads `frames` interleaved frames, blocking until done.
    // Underruns / overruns are recovered transparently and counted. Returns the
    // number of frames transferred or a negative errno.
    snd_pcm_sframes_t Write(const void* buffer, snd_pcm_uframes_t frames);
    snd_pcm_sframes_t Read(void* buffer, snd_pcm_uframes_t frames);

    // State control. All return 0 or a negative errno.
    int Prepare();
    int Start();
    int Drop();
    int Drain();
    int Pause(bool pause);

    snd_pcm_state_t State() const;
    std::optional<PcmPosition> QueryPosition() const;
    // Latency in milliseconds derived from the current delay.
    int32_t LatencyMs() const;
    uint64_t xruns() const { return xruns_; }

  private:
    Pcm(PcmHandle handle, std::string name, snd_pcm_stream_t stream, PcmConfig config, bool is_plug,
        bool can_pause);

    static std::unique_ptr<Pcm> TryOpen(const std::string& name, snd_pcm_stream_t stream,
                                        const PcmConfig& config, bool is_plug);
    // Attempts to recover from an error returned by a transfer. Returns 0 when
    // the transfer may be retried.
    int Recover(int err);

    PcmHandle handle_;
    const std::string name_;
    const snd_pcm_stream_t stream_;
    PcmConfig config_;
    const bool is_plug_;
    const bool can_pause_;
    uint64_t xruns_ = 0;
};

// Probes what the device supports without keeping it open.
std::optional<HwCapabilities> QueryCapabilities(const std::string& name, snd_pcm_stream_t stream);

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
