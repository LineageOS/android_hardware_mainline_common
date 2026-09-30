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
#include "passthrough/PassthroughSink.h"
#include "routing/DeviceInventory.h"
#include "routing/Endpoint.h"
#include "routing/PcmArbiter.h"
#include "routing/RoutingController.h"
#include "stream/NullDevice.h"

namespace aidl::android::hardware::audio::core::mainline {

// Everything a stream needs from the module.
struct StreamDeps {
    std::shared_ptr<routing::DeviceInventory> inventory;
    std::shared_ptr<routing::RoutingController> routing;
    // Shared by all output streams of the module, see routing/PcmArbiter.h.
    std::shared_ptr<routing::PcmArbiter> pcm_arbiter;
    // Shared with the module: when set, captured audio is replaced by silence.
    std::shared_ptr<std::atomic<bool>> mic_muted;
    // Access to the channel status / ELD of HDMI heads, for passthrough.
    passthrough::HdmiControlFactory make_hdmi_control;
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
//
// Output streams open exclusive PCM devices through the PcmArbiter. A direct
// stream takes a device over from a mixed one; the mixed stream then plays
// into the NullDevice for the endpoints it lost (dropping the audio, keeping
// the timing) and reopens them once the direct stream lets go.
//
// A stream with an encoded format (or IEC 61937) is a passthrough stream: it
// only plays to the HDMI template and hands its data to a PassthroughSink
// instead of a PCM device.
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
    ::android::status_t EnsurePassthroughReady();
    bool OpenPcms();
    // Opens one endpoint's PCM into pcms_, or defers it while another stream
    // owns the device. `quiet` limits logging for the retries of a deferred
    // endpoint.
    void OpenEndpoint(const routing::Endpoint& endpoint, const alsa::PcmConfig& config, bool quiet);
    // Asks the arbiter for the device of `endpoint`. False when the endpoint
    // can not be opened now; a mixed stream then keeps it in deferred_endpoints_.
    bool AcquirePcm(const routing::Endpoint& endpoint, bool quiet);
    // Closes the devices another stream asked this one to give up.
    void YieldRequestedPcms();
    // Retries the deferred endpoints.
    void ReopenDeferredPcms();
    void ClosePcms();
    bool IsArbitrated(const alsa::PcmIdentity& pcm) const;
    bool UsingNullDevice() const;
    // True when every endpoint is either null or deferred, i.e. the stream
    // plays into the null device.
    bool OnNullPath() const;
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
    // The mix port carries the FAST flag: the framework runs a FastMixer that
    // expects every write to block for about one burst.
    const bool is_fast_;
    // Set for passthrough streams.
    const std::unique_ptr<passthrough::PassthroughSink> passthrough_;

    std::atomic<float> gain_ = 1.0f;
    // Null for input streams, which are not arbitrated.
    const std::shared_ptr<routing::PcmArbiter::Client> arbiter_client_;

    // Exchanged between Binder threads and the worker thread.
    std::mutex lock_;
    std::vector<routing::Endpoint> connected_endpoints_ GUARDED_BY(lock_);
    std::atomic<bool> endpoints_updated_ = false;

    // Worker thread state.
    std::vector<routing::Endpoint> active_endpoints_;
    struct OpenPcm {
        alsa::PcmIdentity identity;
        std::unique_ptr<alsa::Pcm> pcm;
    };
    std::vector<OpenPcm> pcms_;
    // Endpoints whose device is owned by a direct stream (mixed streams only).
    std::vector<routing::Endpoint> deferred_endpoints_;
    // OpenPcms() ran for active_endpoints_; pcms_ may still be empty when
    // every endpoint is deferred.
    bool pcms_opened_ = false;
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
