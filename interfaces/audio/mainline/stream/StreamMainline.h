/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <android-base/thread_annotations.h>
#include <core-impl/Stream.h>

#include "alsa/AlsaPcm.h"
#include "routing/DeviceInventory.h"
#include "routing/Endpoint.h"
#include "routing/RoutingController.h"
#include "stream/NullDevice.h"

namespace aidl::android::hardware::audio::core::mainline {

// Everything a stream needs from the module.
struct StreamDeps {
    std::shared_ptr<routing::DeviceInventory> inventory;
    std::shared_ptr<routing::RoutingController> routing;
    // Shared with the module: when set, captured audio is replaced by silence.
    std::shared_ptr<std::atomic<bool>> mic_muted;
};

// alsa-lib backed implementation of DriverInterface for both directions.
//
// Threading model (inherited from the example HAL): the AIDL methods
// (setConnectedDevices, setGain, ...) run on Binder threads, everything from
// DriverInterface runs on the stream's worker thread. The two sides talk
// through `connected_endpoints_` (guarded by `lock_`) and the
// `endpoints_updated_` flag; the worker copies the list into
// `active_endpoints_` and (re)opens the PCM devices accordingly.
//
// A stream may be connected to several device ports at once (e.g. speaker and
// headphones for a ringtone); one PCM device is opened per endpoint and the
// same data is written to all of them. Input streams only use the first
// endpoint.
//
// When the connected endpoint is the "null" placeholder (no sound card in the
// system) a NullDevice discards / zero-fills while keeping real-time pacing.
class StreamMainline : public StreamCommonImpl {
  public:
    StreamMainline(StreamContext* context, const Metadata& metadata, StreamDeps deps);
    ~StreamMainline() override;

    // DriverInterface, worker thread.
    ::android::status_t init(DriverCallbackInterface* callback) override;
    ::android::status_t drain(StreamDescriptor::DrainMode mode) override;
    ::android::status_t flush() override;
    ::android::status_t pause() override;
    ::android::status_t standby() override;
    ::android::status_t start() override;
    ::android::status_t transfer(void* buffer, size_t frame_count, size_t* actual_frame_count,
                                 int32_t* latency_ms) override;
    ::android::status_t refinePosition(StreamDescriptor::Position* position) override;
    void shutdown() override;

    // StreamCommonImpl, Binder threads.
    ndk::ScopedAStatus setConnectedDevices(const ConnectedDevices& devices) override;
    ndk::ScopedAStatus setGain(float gain) override;

  private:
    // Resolves AIDL devices to endpoints. Unknown devices are an error.
    std::optional<std::vector<routing::Endpoint>> ResolveEndpoints(const ConnectedDevices& devices);
    // Releases the hardware routing of everything in connected_endpoints_.
    void ReleaseRouting();
    // Worker thread: picks up a new endpoint list posted by setConnectedDevices.
    void ApplyPendingEndpoints();
    // Worker thread: opens the PCM devices (or starts the null device) for the
    // active endpoints if that has not happened yet.
    ::android::status_t EnsureDevicesReady();
    bool OpenPcms();
    void ClosePcms();
    bool UsingNullDevice() const;
    ::android::status_t TransferOutput(void* buffer, size_t frame_count, int32_t* latency_ms);
    ::android::status_t TransferInput(void* buffer, size_t frame_count, int32_t* latency_ms);
    alsa::PcmConfig MakePcmConfig() const;
    const char* Tag() const { return is_input_ ? "[in] " : "[out] "; }

    const StreamDeps deps_;
    const bool is_input_;
    const size_t frame_size_bytes_;
    const size_t buffer_size_frames_;
    const unsigned int channel_count_;
    const std::optional<snd_pcm_format_t> alsa_format_;

    std::atomic<float> gain_ = 1.0f;

    // Exchanged between Binder threads and the worker thread.
    std::mutex lock_;
    std::vector<routing::Endpoint> connected_endpoints_ GUARDED_BY(lock_);
    std::atomic<bool> endpoints_updated_ = false;

    // Worker thread state.
    std::vector<routing::Endpoint> active_endpoints_;
    std::vector<std::unique_ptr<alsa::Pcm>> pcms_;
    NullDevice null_device_;
    bool null_running_ = false;
};

class StreamInMainline final : public StreamIn, public StreamMainline {
  public:
    friend class ndk::SharedRefBase;
    StreamInMainline(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SinkMetadata& sink_metadata,
            const std::vector<::aidl::android::media::audio::common::MicrophoneInfo>& microphones,
            StreamDeps deps);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

class StreamOutMainline final : public StreamOut, public StreamMainline {
  public:
    friend class ndk::SharedRefBase;
    StreamOutMainline(
            StreamContext&& context,
            const ::aidl::android::hardware::audio::common::SourceMetadata& source_metadata,
            const std::optional<::aidl::android::media::audio::common::AudioOffloadInfo>&
                    offload_info,
            StreamDeps deps);

  private:
    void onClose(StreamDescriptor::State) override { defaultOnClose(); }
};

}  // namespace aidl::android::hardware::audio::core::mainline
