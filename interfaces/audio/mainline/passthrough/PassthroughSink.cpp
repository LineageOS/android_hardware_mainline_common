/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Passthrough"

#include "passthrough/PassthroughSink.h"

#include <algorithm>
#include <cerrno>

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core::mainline::passthrough {

namespace {

constexpr snd_pcm_uframes_t kMinPeriodFrames = 64;

}  // namespace

PassthroughSink::PassthroughSink(Config config, HdmiControlFactory make_control)
    : config_(config), make_control_(std::move(make_control)) {}

PassthroughSink::~PassthroughSink() {
    Close();
}

bool PassthroughSink::IsPossible(const Config& config) {
    if (config.format != EncodedFormat::kIec61937 && !Encoder::CanPack(config.format)) {
        return false;
    }
    return IecStreamFor(config.format, config.content_rate, config.channels, true /*hbr*/)
            .has_value();
}

bool PassthroughSink::Open(const routing::Endpoint& head) {
    Close();
    control_ = make_control_ != nullptr ? make_control_(head) : nullptr;
    if (control_ == nullptr) {
        LOG(ERROR) << __func__ << ": no passthrough on " << head.ToString();
        return false;
    }
    const bool hbr = control_->Hbr() == HbrSupport::kSupported;
    iec_ = IecStreamFor(config_.format, config_.content_rate, config_.channels, hbr);
    if (!iec_.has_value()) {
        LOG(ERROR) << __func__ << ": " << ToString(config_.format) << " at " << config_.content_rate
                   << " Hz, " << config_.channels << " channels can not be sent to "
                   << control_->Describe();
        Close();
        return false;
    }
    if (config_.format != EncodedFormat::kIec61937) {
        encoder_ = Encoder::Create(
                config_.format, *iec_,
                [this](const uint8_t* data, size_t bytes) { WriteBursts(data, bytes); });
        if (encoder_ == nullptr || encoder_->OutputChannels() != iec_->pcm_channels) {
            LOG(ERROR) << __func__ << ": no packer for " << ToString(config_.format) << " as "
                       << iec_->ToString();
            Close();
            return false;
        }
    }
    // Before the PCM device is opened: HDA takes the non-audio flag into the
    // stream format when the device is prepared, which Pcm::OpenStrict does.
    status_guard_ = control_->SetChannelStatus(NonAudioChannelStatus(*iec_));
    if (status_guard_ == nullptr) {
        LOG(ERROR) << __func__ << ": could not set the channel status of " << control_->Describe();
        Close();
        return false;
    }

    alsa::PcmConfig pcm_config;
    pcm_config.format = SND_PCM_FORMAT_S16_LE;
    pcm_config.channels = iec_->pcm_channels;
    pcm_config.rate = iec_->pcm_rate;
    pcm_config.period_frames = std::max<snd_pcm_uframes_t>(
            iec_->pcm_rate * config_.latency_ms / 2000, kMinPeriodFrames);
    pcm_config.buffer_frames = pcm_config.period_frames * 4;
    int err = 0;
    pcm_ = alsa::Pcm::OpenStrict(head.pcm_name, SND_PCM_STREAM_PLAYBACK, pcm_config, &err);
    if (pcm_ == nullptr) {
        // HDA refuses a high bit rate stream at prepare time when the pin or
        // the display side can not do it.
        if (iec_->hbr && err == -EINVAL) control_->ReportHbrFailure();
        Close();
        return false;
    }
    pcm_frames_written_ = 0;
    partial_frame_.clear();
    LOG(INFO) << __func__ << ": " << ToString(config_.format) << " at " << config_.content_rate
              << " Hz as " << iec_->ToString() << " on " << control_->Describe();
    return true;
}

void PassthroughSink::WriteBursts(const uint8_t* data, size_t bytes) {
    if (pcm_ == nullptr || write_failed_) return;
    const size_t frame_bytes = iec_->pcm_channels * sizeof(int16_t);
    if (!partial_frame_.empty()) {
        const size_t take = std::min(frame_bytes - partial_frame_.size(), bytes);
        partial_frame_.insert(partial_frame_.end(), data, data + take);
        data += take;
        bytes -= take;
        if (partial_frame_.size() < frame_bytes) return;
        if (pcm_->Write(partial_frame_.data(), 1) < 0) {
            write_failed_ = true;
            return;
        }
        ++pcm_frames_written_;
        partial_frame_.clear();
    }
    const size_t frames = bytes / frame_bytes;
    if (frames > 0) {
        const snd_pcm_sframes_t written = pcm_->Write(data, frames);
        if (written < 0) {
            LOG(WARNING) << __func__ << ": write to " << pcm_->name() << " failed: " << written;
            write_failed_ = true;
            return;
        }
        pcm_frames_written_ += static_cast<int64_t>(frames);
    }
    const size_t rest = bytes - frames * frame_bytes;
    partial_frame_.assign(data + frames * frame_bytes, data + frames * frame_bytes + rest);
}

bool PassthroughSink::Write(const void* data, size_t bytes, int32_t* latency_ms) {
    if (pcm_ == nullptr) return false;
    write_failed_ = false;
    if (encoder_ != nullptr) {
        encoder_->Write(data, bytes);
    } else {
        WriteBursts(static_cast<const uint8_t*>(data), bytes);
    }
    if (write_failed_) return false;
    *latency_ms = pcm_->LatencyMs();
    return true;
}

void PassthroughSink::Drain() {
    if (pcm_ == nullptr) return;
    pcm_->Drain();
    pcm_->Prepare();
    if (encoder_ != nullptr) encoder_->Reset();
    partial_frame_.clear();
}

void PassthroughSink::Flush() {
    if (pcm_ == nullptr) return;
    int64_t time_ns = 0;
    // What is still queued is dropped, so it never counts as presented.
    presented_before_ = PresentedFrames(&time_ns);
    pcm_frames_written_ = 0;
    pcm_->Drop();
    pcm_->Prepare();
    if (encoder_ != nullptr) encoder_->Reset();
    partial_frame_.clear();
}

void PassthroughSink::Close() {
    if (pcm_ != nullptr) {
        int64_t time_ns = 0;
        // What is still queued is dropped, so it never counts as presented.
        presented_before_ = PresentedFrames(&time_ns);
        pcm_frames_written_ = 0;
    }
    // Close the device before the channel status goes back to audio.
    pcm_.reset();
    status_guard_.reset();
    encoder_.reset();
    control_.reset();
    iec_.reset();
    partial_frame_.clear();
}

int64_t PassthroughSink::ToContentFrames(int64_t pcm_frames) const {
    if (!iec_.has_value() || iec_->pcm_rate == 0) return 0;
    return pcm_frames * config_.content_rate / iec_->pcm_rate;
}

int64_t PassthroughSink::PresentedFrames(int64_t* time_ns) {
    int64_t presented = presented_before_;
    if (pcm_ != nullptr) {
        if (const auto position = pcm_->QueryPosition(); position.has_value()) {
            presented += ToContentFrames(
                    std::max<int64_t>(0, pcm_frames_written_ - position->delay_frames));
            *time_ns = position->time_ns;
        }
    }
    last_presented_ = std::max(last_presented_, presented);
    return last_presented_;
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
