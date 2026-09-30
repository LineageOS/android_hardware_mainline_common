/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "alsa/AlsaPcm.h"
#include "passthrough/Encoder.h"
#include "passthrough/Format.h"
#include "passthrough/HdmiControl.h"
#include "routing/Endpoint.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// Creates the HdmiControl of an HDMI head, nullptr when passthrough is not
// possible on it. Supplied by the module, so that streams stay vendor neutral.
using HdmiControlFactory =
        std::function<std::unique_ptr<HdmiControl>(const routing::Endpoint& head)>;

// The output of a passthrough stream: packs the encoded stream (unless it is
// IEC 61937 already), sets the head's channel status to non-audio and writes
// the bursts to the head's PCM device, opened without any conversion.
// Used from the stream's worker thread only. Arbitration of the PCM device is
// the stream's business.
class PassthroughSink {
  public:
    struct Config {
        EncodedFormat format = EncodedFormat::kIec61937;
        // Sample rate and channel count of the stream as the framework sees it.
        uint32_t content_rate = 0;
        unsigned int channels = 0;
        // Nominal latency, sizes the PCM buffer.
        int32_t latency_ms = 0;
    };

    PassthroughSink(Config config, HdmiControlFactory make_control);
    ~PassthroughSink();

    // Whether this build can pack the format and an IEC 61937 stream exists
    // for the configuration at all, with high bit rate if the head allows it.
    static bool IsPossible(const Config& config);

    // Opens `head`. Fails when the head has no controls, can not do the
    // needed rate / high bit rate, or refuses the PCM configuration; a high
    // bit rate refusal is reported to the head's HdmiControl.
    bool Open(const routing::Endpoint& head);
    bool IsOpen() const { return pcm_ != nullptr; }
    // Takes `bytes` of the stream, all of them unless the device fails.
    bool Write(const void* data, size_t bytes, int32_t* latency_ms);
    // Plays what is queued. A partial burst still in the packer is dropped.
    void Drain();
    // Drops what is queued and what the packer holds, keeps the device.
    void Flush();
    // Closes the device and restores the channel status; queued data is
    // dropped. The next Write() needs another Open().
    void Close();
    // Frames of the stream presented so far, at the content rate: what an
    // application expects from a direct encoded output. For IEC 61937 input,
    // frames of the stream as written. Monotonic across Close() / Open().
    // `time_ns` gets the time the device position was sampled at, and is left
    // alone when no device is open.
    int64_t PresentedFrames(int64_t* time_ns);

  private:
    void WriteBursts(const uint8_t* data, size_t bytes);
    int64_t ToContentFrames(int64_t pcm_frames) const;

    const Config config_;
    const HdmiControlFactory make_control_;

    std::optional<IecStream> iec_;
    std::unique_ptr<HdmiControl> control_;
    std::unique_ptr<ChannelStatusGuard> status_guard_;
    std::unique_ptr<alsa::Pcm> pcm_;
    std::unique_ptr<Encoder> encoder_;
    // Bytes of an incomplete PCM frame, waiting for the rest.
    std::vector<uint8_t> partial_frame_;
    bool write_failed_ = false;

    // PCM frames written since Open().
    int64_t pcm_frames_written_ = 0;
    // Content frames presented before the current Open().
    int64_t presented_before_ = 0;
    int64_t last_presented_ = 0;
};

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
