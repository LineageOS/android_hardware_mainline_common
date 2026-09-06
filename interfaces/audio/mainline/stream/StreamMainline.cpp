/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Stream"

#include "stream/StreamMainline.h"

#include <algorithm>
#include <cstring>

#include <Log.h>
#include <Utils.h>
#include <error/Result.h>
#include <error/expected_utils.h>

#include "alsa/AlsaError.h"
#include "alsa/AlsaFormat.h"

namespace aidl::android::hardware::audio::core::mainline {

using ::aidl::android::hardware::audio::common::SinkMetadata;
using ::aidl::android::hardware::audio::common::SourceMetadata;
using ::aidl::android::media::audio::common::AudioDevice;
using ::aidl::android::media::audio::common::AudioOffloadInfo;
using ::aidl::android::media::audio::common::MicrophoneInfo;

StreamMainline::StreamMainline(StreamContext* context, const Metadata& metadata, StreamDeps deps)
    : StreamCommonImpl(context, metadata),
      deps_(std::move(deps)),
      is_input_(isInput(metadata)),
      frame_size_bytes_(getContext().getFrameSize()),
      buffer_size_frames_(getContext().getBufferSizeInFrames()),
      channel_count_(alsa::ChannelCount(getContext().getChannelLayout())),
      alsa_format_(alsa::ToAlsaFormat(getContext().getFormat())),
      null_device_(getContext().getSampleRate()) {
    LOG(DEBUG) << Tag() << __func__ << ": format=" << getContext().getFormat().toString()
               << " channels=" << getContext().getChannelLayout().toString()
               << " rate=" << getContext().getSampleRate()
               << " bufferFrames=" << buffer_size_frames_ << " frameSize=" << frame_size_bytes_;
}

StreamMainline::~StreamMainline() {
    cleanupWorker();
    // shutdown() normally releases the routing; cover the case where the worker
    // never ran.
    ReleaseRouting();
}

// --- Binder thread side ------------------------------------------------------

std::optional<std::vector<routing::Endpoint>> StreamMainline::ResolveEndpoints(
        const ConnectedDevices& devices) {
    std::vector<routing::Endpoint> endpoints;
    for (const AudioDevice& device : devices) {
        if (const routing::Endpoint* e = deps_.inventory->FindByDevice(device); e != nullptr) {
            if (e->is_input != is_input_) {
                LOG(ERROR) << Tag() << __func__ << ": direction mismatch for " << device.toString();
                return std::nullopt;
            }
            endpoints.push_back(*e);
            continue;
        }
        if (auto usb = deps_.inventory->MakeUsbEndpoint(device, is_input_); usb.has_value()) {
            endpoints.push_back(std::move(*usb));
            continue;
        }
        LOG(ERROR) << Tag() << __func__ << ": no endpoint for device " << device.toString();
        return std::nullopt;
    }
    return endpoints;
}

void StreamMainline::ReleaseRouting() {
    std::lock_guard guard(lock_);
    for (const auto& endpoint : connected_endpoints_) deps_.routing->Release(endpoint);
    connected_endpoints_.clear();
}

ndk::ScopedAStatus StreamMainline::setConnectedDevices(const ConnectedDevices& devices) {
    if (is_input_ && devices.size() > 1) {
        LOG(ERROR) << Tag() << __func__ << ": input streams support a single device, got "
                   << devices.size();
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    auto endpoints = ResolveEndpoints(devices);
    if (!endpoints.has_value()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    {
        // Switch the hardware routing right away: the worker only picks the
        // new PCM devices up on its next transfer, but enabling the UCM device
        // early gives the codec time to settle. Acquire before release so that
        // a device shared by the old and the new set never gets toggled.
        std::lock_guard guard(lock_);
        for (const auto& endpoint : *endpoints) deps_.routing->Acquire(endpoint);
        for (const auto& endpoint : connected_endpoints_) deps_.routing->Release(endpoint);
        connected_endpoints_ = std::move(*endpoints);
        LOG(INFO) << Tag() << __func__ << ": routed to " << connected_endpoints_.size()
                  << " endpoint(s)";
        for (const auto& e : connected_endpoints_) LOG(INFO) << Tag() << "  " << e.ToString();
    }
    // Publish the new list before the base class flips the worker's
    // "connected" flag, otherwise the worker could transfer without devices.
    endpoints_updated_.store(true, std::memory_order_release);
    return StreamCommonImpl::setConnectedDevices(devices);
}

ndk::ScopedAStatus StreamMainline::setGain(float gain) {
    gain_.store(gain, std::memory_order_relaxed);
    return ndk::ScopedAStatus::ok();
}

// --- Worker thread side ------------------------------------------------------

bool StreamMainline::UsingNullDevice() const {
    return !active_endpoints_.empty() &&
           std::all_of(active_endpoints_.begin(), active_endpoints_.end(),
                       [](const routing::Endpoint& e) { return e.IsNull(); });
}

alsa::PcmConfig StreamMainline::MakePcmConfig() const {
    alsa::PcmConfig config;
    config.format = alsa_format_.value_or(SND_PCM_FORMAT_S16_LE);
    config.channels = channel_count_;
    config.rate = static_cast<unsigned int>(getContext().getSampleRate());
    // Wake up twice per framework burst, keep two bursts of headroom.
    config.period_frames = std::max<snd_pcm_uframes_t>(buffer_size_frames_ / 2, 64);
    config.buffer_frames = buffer_size_frames_ * 2;
    return config;
}

bool StreamMainline::OpenPcms() {
    ClosePcms();
    const alsa::PcmConfig config = MakePcmConfig();
    const snd_pcm_stream_t direction = is_input_ ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
    for (const routing::Endpoint& endpoint : active_endpoints_) {
        if (endpoint.IsNull()) continue;
        auto pcm = alsa::Pcm::Open(endpoint.pcm_name, direction, config);
        if (pcm == nullptr) {
            LOG(ERROR) << Tag() << __func__ << ": failed to open " << endpoint.ToString();
            continue;
        }
        // Pcm::Open() hands out a prepared device.
        pcms_.push_back(std::move(pcm));
        if (is_input_) break;  // A capture stream reads from one device only.
    }
    if (pcms_.empty()) {
        LOG(ERROR) << Tag() << __func__ << ": no PCM device could be opened for "
                   << active_endpoints_.size() << " endpoint(s)";
        return false;
    }
    return true;
}

void StreamMainline::ClosePcms() {
    pcms_.clear();
}

void StreamMainline::ApplyPendingEndpoints() {
    if (!endpoints_updated_.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard guard(lock_);
        active_endpoints_ = connected_endpoints_;
    }
    // Devices are reopened lazily by EnsureDevicesReady(), on the next start
    // or transfer. For a running stream this happens within the same burst,
    // which is what a patch update requires.
    ClosePcms();
    null_running_ = false;
}

::android::status_t StreamMainline::EnsureDevicesReady() {
    if (active_endpoints_.empty()) {
        // Not patched yet. The worker does not transfer in this case.
        return ::android::OK;
    }
    if (UsingNullDevice()) {
        if (!null_running_) {
            null_device_.Start();
            null_running_ = true;
        }
        return ::android::OK;
    }
    if (pcms_.empty() && !OpenPcms()) return ::android::NO_INIT;
    // Capture has to be kicked explicitly; playback starts on its own once the
    // start threshold is reached. Only touch devices that are actually idle,
    // resuming from PAUSED lands here too.
    if (is_input_) {
        for (auto& pcm : pcms_) {
            if (pcm->State() == SND_PCM_STATE_PREPARED) pcm->Start();
        }
    }
    return ::android::OK;
}

::android::status_t StreamMainline::init(DriverCallbackInterface* /*callback*/) {
    if (!alsa_format_.has_value()) {
        LOG(ERROR) << Tag() << __func__ << ": unsupported format "
                   << getContext().getFormat().toString();
        return ::android::NO_INIT;
    }
    if (channel_count_ == 0) {
        LOG(ERROR) << Tag() << __func__ << ": invalid channel layout "
                   << getContext().getChannelLayout().toString();
        return ::android::NO_INIT;
    }
    return ::android::OK;
}

::android::status_t StreamMainline::start() {
    ApplyPendingEndpoints();
    return EnsureDevicesReady();
}

::android::status_t StreamMainline::standby() {
    ClosePcms();
    null_running_ = false;
    return ::android::OK;
}

::android::status_t StreamMainline::drain(StreamDescriptor::DrainMode /*mode*/) {
    if (is_input_) return ::android::OK;
    if (UsingNullDevice()) {
        null_device_.Transfer(nullptr, buffer_size_frames_, frame_size_bytes_, false);
        return ::android::OK;
    }
    for (auto& pcm : pcms_) {
        // snd_pcm_drain() blocks until the queued data has been played.
        pcm->Drain();
        pcm->Prepare();
    }
    return ::android::OK;
}

::android::status_t StreamMainline::flush() {
    for (auto& pcm : pcms_) {
        pcm->Drop();
        pcm->Prepare();
    }
    return ::android::OK;
}

::android::status_t StreamMainline::pause() {
    // Playback is left to run dry (the ring buffer holds at most two bursts),
    // capture keeps filling the hardware buffer and drops on overrun. Both
    // match what the state machine expects from the PAUSED state.
    return ::android::OK;
}

::android::status_t StreamMainline::TransferOutput(void* buffer, size_t frame_count,
                                                   int32_t* latency_ms) {
    const snd_pcm_format_t format = *alsa_format_;
    alsa::ApplyGain(buffer, frame_count, channel_count_, format,
                    gain_.load(std::memory_order_relaxed));
    alsa::ReorderChannelsAndroidToAlsa(buffer, frame_count, channel_count_, format,
                                       getContext().getChannelLayout());
    int32_t latency = 0;
    bool any_ok = false;
    for (auto& pcm : pcms_) {
        const snd_pcm_sframes_t written = pcm->Write(buffer, frame_count);
        if (written < 0) {
            LOG(WARNING) << Tag() << __func__ << ": write to " << pcm->name()
                         << " failed: " << alsa::ErrorString(static_cast<int>(written));
            continue;
        }
        any_ok = true;
        latency = std::max(latency, pcm->LatencyMs());
    }
    if (!any_ok) return ::android::INVALID_OPERATION;
    *latency_ms = latency;
    return ::android::OK;
}

::android::status_t StreamMainline::TransferInput(void* buffer, size_t frame_count,
                                                  int32_t* latency_ms) {
    alsa::Pcm& pcm = *pcms_.front();
    const snd_pcm_sframes_t read = pcm.Read(buffer, frame_count);
    if (read < 0) {
        LOG(WARNING) << Tag() << __func__ << ": read from " << pcm.name()
                     << " failed: " << alsa::ErrorString(static_cast<int>(read));
        return ::android::INVALID_OPERATION;
    }
    alsa::ReorderChannelsAlsaToAndroid(buffer, frame_count, channel_count_, *alsa_format_,
                                       getContext().getChannelLayout());
    if (deps_.mic_muted != nullptr && deps_.mic_muted->load(std::memory_order_relaxed)) {
        std::memset(buffer, 0, frame_count * frame_size_bytes_);
    }
    *latency_ms = pcm.LatencyMs();
    return ::android::OK;
}

::android::status_t StreamMainline::transfer(void* buffer, size_t frame_count,
                                             size_t* actual_frame_count, int32_t* latency_ms) {
    *actual_frame_count = 0;
    // Routing may have changed underneath a running stream, and a burst may
    // arrive in STANDBY without a preceding start().
    ApplyPendingEndpoints();
    RETURN_STATUS_IF_ERROR(EnsureDevicesReady());
    if (UsingNullDevice()) {
        null_device_.Transfer(buffer, frame_count, frame_size_bytes_, is_input_);
        *actual_frame_count = frame_count;
        *latency_ms = null_device_.LatencyMs(buffer_size_frames_);
        return ::android::OK;
    }
    if (pcms_.empty()) {
        LOG(ERROR) << Tag() << __func__ << ": no PCM device is open";
        *latency_ms = StreamDescriptor::LATENCY_UNKNOWN;
        return ::android::NO_INIT;
    }
    const ::android::status_t status = is_input_ ? TransferInput(buffer, frame_count, latency_ms)
                                                 : TransferOutput(buffer, frame_count, latency_ms);
    if (status != ::android::OK) {
        *latency_ms = StreamDescriptor::LATENCY_UNKNOWN;
        return status;
    }
    *actual_frame_count = frame_count;
    return ::android::OK;
}

::android::status_t StreamMainline::refinePosition(StreamDescriptor::Position* position) {
    if (UsingNullDevice() || pcms_.empty()) return ::android::OK;
    const auto pcm_position = pcms_.front()->QueryPosition();
    if (!pcm_position.has_value()) return ::android::OK;
    if (is_input_) {
        // Frames captured by the hardware = frames handed to the client plus
        // what is still waiting in the ring buffer.
        position->frames += pcm_position->delay_frames;
    } else {
        // Frames presented = frames written minus what is still queued.
        position->frames = std::max<int64_t>(0, position->frames - pcm_position->delay_frames);
    }
    position->timeNs = pcm_position->time_ns;
    return ::android::OK;
}

void StreamMainline::shutdown() {
    ClosePcms();
    null_running_ = false;
    active_endpoints_.clear();
    ReleaseRouting();
}

// --- Concrete streams --------------------------------------------------------

StreamInMainline::StreamInMainline(StreamContext&& context, const SinkMetadata& sink_metadata,
                                   const std::vector<MicrophoneInfo>& microphones, StreamDeps deps)
    : StreamIn(std::move(context), microphones),
      StreamMainline(&mContextInstance, sink_metadata, std::move(deps)) {}

StreamOutMainline::StreamOutMainline(StreamContext&& context, const SourceMetadata& source_metadata,
                                     const std::optional<AudioOffloadInfo>& offload_info,
                                     StreamDeps deps)
    : StreamOut(std::move(context), offload_info),
      StreamMainline(&mContextInstance, source_metadata, std::move(deps)) {}

}  // namespace aidl::android::hardware::audio::core::mainline
