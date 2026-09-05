/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_AlsaPcm"

#include "alsa/AlsaPcm.h"

#include <unistd.h>

#include <cerrno>
#include <ctime>
#include <sstream>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>

namespace aidl::android::hardware::audio::core::mainline::alsa {

namespace {

constexpr int kMaxRecoverAttempts = 5;
constexpr unsigned int kSuspendRetryDelayUs = 10 * 1000;
constexpr int64_t kNsPerSec = 1000000000LL;

int64_t TimespecToNs(const snd_htimestamp_t& ts) {
    return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

int64_t NowMonotonicNs() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

// Applies `config` to `pcm`. On success `effective` holds what the driver
// actually accepted. `strict` requires the exact format / rate / channels.
int ConfigureHwParams(snd_pcm_t* pcm, const PcmConfig& config, bool strict, PcmConfig* effective,
                      bool* can_pause) {
    HwParamsPtr params = AllocHwParams();
    int err = snd_pcm_hw_params_any(pcm, params.get());
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_pcm_hw_params_any: " << ErrorString(err);
        return err;
    }
    // Let the plug layer resample when it is in the path; for "hw:" this is a
    // no-op.
    snd_pcm_hw_params_set_rate_resample(pcm, params.get(), strict ? 0 : 1);

    err = snd_pcm_hw_params_set_access(pcm, params.get(), SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        LOG(WARNING) << __func__ << ": set_access(RW_INTERLEAVED): " << ErrorString(err);
        return err;
    }
    err = snd_pcm_hw_params_set_format(pcm, params.get(), config.format);
    if (err < 0) {
        LOG(DEBUG) << __func__ << ": set_format(" << snd_pcm_format_name(config.format)
                   << "): " << ErrorString(err);
        return err;
    }
    err = snd_pcm_hw_params_set_channels(pcm, params.get(), config.channels);
    if (err < 0) {
        LOG(DEBUG) << __func__ << ": set_channels(" << config.channels << "): " << ErrorString(err);
        return err;
    }
    unsigned int rate = config.rate;
    err = snd_pcm_hw_params_set_rate_near(pcm, params.get(), &rate, nullptr);
    if (err < 0) {
        LOG(DEBUG) << __func__ << ": set_rate_near(" << config.rate << "): " << ErrorString(err);
        return err;
    }
    if (strict && rate != config.rate) {
        LOG(DEBUG) << __func__ << ": rate " << config.rate << " not supported, closest is " << rate;
        return -EINVAL;
    }

    snd_pcm_uframes_t period = config.period_frames;
    if (period > 0) {
        err = snd_pcm_hw_params_set_period_size_near(pcm, params.get(), &period, nullptr);
        if (err < 0) {
            LOG(WARNING) << __func__ << ": set_period_size_near(" << config.period_frames
                         << "): " << ErrorString(err);
            return err;
        }
    }
    // Express the buffer as a number of periods rather than as a size of its
    // own. Drivers constrain the period size (the Qualcomm q6asm front-end
    // only accepts multiples of 480 frames) and require an integer number of
    // periods, so a buffer size chosen independently of the period that was
    // granted can fail to be a multiple of it and make hw_params() fail.
    if (config.buffer_frames > 0 &&
        snd_pcm_hw_params_get_period_size(params.get(), &period, nullptr) == 0 && period > 0) {
        unsigned int periods =
                static_cast<unsigned int>((config.buffer_frames + period / 2) / period);
        if (periods < 2) periods = 2;
        err = snd_pcm_hw_params_set_periods_near(pcm, params.get(), &periods, nullptr);
        if (err < 0) {
            LOG(WARNING) << __func__ << ": set_periods_near(" << periods
                         << "): " << ErrorString(err);
            return err;
        }
    } else if (config.buffer_frames > 0) {
        snd_pcm_uframes_t buffer = config.buffer_frames;
        err = snd_pcm_hw_params_set_buffer_size_near(pcm, params.get(), &buffer);
        if (err < 0) {
            LOG(WARNING) << __func__ << ": set_buffer_size_near(" << config.buffer_frames
                         << "): " << ErrorString(err);
            return err;
        }
    }

    err = snd_pcm_hw_params(pcm, params.get());
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_pcm_hw_params({" << config.ToString()
                     << "}): " << ErrorString(err);
        return err;
    }

    *effective = config;
    effective->rate = rate;
    snd_pcm_hw_params_get_period_size(params.get(), &effective->period_frames, nullptr);
    snd_pcm_hw_params_get_buffer_size(params.get(), &effective->buffer_frames);
    *can_pause = snd_pcm_hw_params_can_pause(params.get()) != 0;
    return 0;
}

int ConfigureSwParams(snd_pcm_t* pcm, snd_pcm_stream_t stream, const PcmConfig& effective) {
    SwParamsPtr params = AllocSwParams();
    int err = snd_pcm_sw_params_current(pcm, params.get());
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_pcm_sw_params_current: " << ErrorString(err);
        return err;
    }
    // Playback starts once a full period is queued, capture starts on the
    // first read (start threshold of 1 frame).
    const snd_pcm_uframes_t start_threshold =
            stream == SND_PCM_STREAM_PLAYBACK ? effective.period_frames : 1;
    snd_pcm_sw_params_set_start_threshold(pcm, params.get(), start_threshold);
    snd_pcm_sw_params_set_avail_min(pcm, params.get(), effective.period_frames);
    // The stop threshold is left at its default (the buffer size), so an
    // underrun / overrun surfaces as -EPIPE from the transfer call and is
    // recovered in Pcm::Recover().
    snd_pcm_sw_params_set_tstamp_mode(pcm, params.get(), SND_PCM_TSTAMP_ENABLE);
    snd_pcm_sw_params_set_tstamp_type(pcm, params.get(), SND_PCM_TSTAMP_TYPE_MONOTONIC);
    err = snd_pcm_sw_params(pcm, params.get());
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_pcm_sw_params: " << ErrorString(err);
    }
    return err;
}

std::string UnderlyingPcmName(const std::string& name) {
    // alsa-lib hands out the devices of a use case as "_ucmXXXX.<name>" and
    // resolves that prefix against the private configuration of the use case
    // manager. The slave of a plugin is resolved against the global
    // configuration instead, where the prefixed name does not exist, so the
    // fallbacks have to name the underlying device. The mixer setup of the
    // use case is applied by UcmManager and is not affected.
    static constexpr char kUcmPrefix[] = "_ucm";
    static constexpr size_t kUcmPrefixLength = 9;  // "_ucm" + 4 hex digits + '.'
    if (name.size() > kUcmPrefixLength && ::android::base::StartsWith(name, kUcmPrefix) &&
        name[kUcmPrefixLength - 1] == '.') {
        return name.substr(kUcmPrefixLength);
    }
    return name;
}

// A "plug" PCM on top of `slave`: it converts the sample format, the channel
// count and the sample rate to what the hardware accepts. A non-zero
// `slave_rate` pins the rate used on the hardware side.
std::string PlugName(const std::string& slave, unsigned int slave_rate) {
    std::ostringstream os;
    os << "plug:{SLAVE={pcm \"" << slave << "\"";
    if (slave_rate != 0) os << " rate " << slave_rate;
    os << "}}";
    return os.str();
}

// Hardware rates to try when the device turns out to reject the rate the plug
// layer picked, the most commonly implemented ones first.
std::vector<unsigned int> SlaveRateCandidates(unsigned int rate) {
    static constexpr unsigned int kPreferred[] = {48000, 44100, 96000, 192000};
    std::vector<unsigned int> candidates;
    for (const unsigned int candidate : kPreferred) {
        if (candidate != rate) candidates.push_back(candidate);
    }
    return candidates;
}

}  // namespace

// --- PcmConfig ---------------------------------------------------------------

std::string PcmConfig::ToString() const {
    std::ostringstream os;
    os << snd_pcm_format_name(format) << " " << channels << "ch " << rate
       << "Hz period=" << period_frames << " buffer=" << buffer_frames;
    return os.str();
}

// --- Pcm ---------------------------------------------------------------------

Pcm::Pcm(PcmHandle handle, std::string name, snd_pcm_stream_t stream, PcmConfig config,
         bool is_plug, bool can_pause)
    : handle_(std::move(handle)),
      name_(std::move(name)),
      stream_(stream),
      config_(config),
      is_plug_(is_plug),
      can_pause_(can_pause) {}

Pcm::~Pcm() {
    if (handle_ != nullptr) {
        LOG(DEBUG) << "closing " << name_ << " (" << snd_pcm_stream_name(stream_)
                   << "), xruns=" << xruns_;
    }
}

std::unique_ptr<Pcm> Pcm::TryOpen(const std::string& name, snd_pcm_stream_t stream,
                                  const PcmConfig& config, bool is_plug) {
    snd_pcm_t* raw = nullptr;
    int err = snd_pcm_open(&raw, name.c_str(), stream, 0 /*blocking*/);
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_pcm_open(" << name << ", " << snd_pcm_stream_name(stream)
                     << "): " << ErrorString(err);
        return nullptr;
    }
    PcmHandle handle(raw);

    PcmConfig effective;
    bool can_pause = false;
    err = ConfigureHwParams(raw, config, !is_plug /*strict*/, &effective, &can_pause);
    if (err < 0) {
        return nullptr;
    }
    if (ConfigureSwParams(raw, stream, effective) < 0) {
        return nullptr;
    }
    LOG(INFO) << __func__ << ": opened " << name << " (" << snd_pcm_stream_name(stream)
              << ") requested={" << config.ToString() << "} effective={" << effective.ToString()
              << "} can_pause=" << can_pause;
    return std::unique_ptr<Pcm>(
            new Pcm(std::move(handle), name, stream, effective, is_plug, can_pause));
}

std::unique_ptr<Pcm> Pcm::Open(const std::string& name, snd_pcm_stream_t stream,
                               const PcmConfig& config) {
    if (auto pcm = TryOpen(name, stream, config, false /*is_plug*/); pcm != nullptr) {
        return pcm;
    }
    // The device did not accept the configuration as it is. Retry through the
    // plug layer, which converts the sample format, the channel count and the
    // sample rate to something the hardware does accept.
    const std::string slave = UnderlyingPcmName(name);
    LOG(INFO) << __func__ << ": " << name << " does not accept {" << config.ToString()
              << "} natively, retrying through the plug layer";
    if (auto pcm = TryOpen(PlugName(slave, 0), stream, config, true /*is_plug*/); pcm != nullptr) {
        return pcm;
    }

    // Still refused. What a device answers to hw_params_any() is not
    // necessarily what it accepts in hw_params(): on a DPCM card (every
    // Qualcomm QDSP6 one) the front-end answers the queries on its own and the
    // constraints of the back-end it is routed to are only applied on commit.
    // The plug layer picks the hardware rate from those same answers, so it
    // can pick one that the back-end rejects; pin it to a rate that is
    // commonly implemented instead.
    for (const unsigned int slave_rate : SlaveRateCandidates(config.rate)) {
        if (auto pcm = TryOpen(PlugName(slave, slave_rate), stream, config, true /*is_plug*/);
            pcm != nullptr) {
            LOG(INFO) << __func__ << ": " << name << " runs at " << slave_rate
                      << " Hz, converting from " << config.rate << " Hz in the plug layer";
            return pcm;
        }
    }

    LOG(ERROR) << __func__ << ": " << name << " does not accept {" << config.ToString()
               << "} in any configuration, giving up";
    return nullptr;
}

int Pcm::Recover(int err) {
    if (err == -EPIPE) {
        ++xruns_;
        LOG(WARNING) << name_ << ": "
                     << (stream_ == SND_PCM_STREAM_PLAYBACK ? "underrun" : "overrun") << " #"
                     << xruns_;
    } else if (err == -ESTRPIPE) {
        LOG(WARNING) << name_ << ": device suspended, waiting for resume";
        int resume_err = 0;
        for (int attempt = 0; attempt < 100; ++attempt) {
            resume_err = snd_pcm_resume(handle_.get());
            if (resume_err != -EAGAIN) break;
            usleep(kSuspendRetryDelayUs);
        }
        if (resume_err == 0) return 0;
    } else if (err != -EINTR) {
        LOG(ERROR) << name_ << ": transfer error: " << ErrorString(err);
        return err;
    }
    const int prepare_err = snd_pcm_prepare(handle_.get());
    if (prepare_err < 0) {
        LOG(ERROR) << name_ << ": snd_pcm_prepare after error failed: " << ErrorString(prepare_err);
        return prepare_err;
    }
    return 0;
}

snd_pcm_sframes_t Pcm::Write(const void* buffer, snd_pcm_uframes_t frames) {
    const uint8_t* data = static_cast<const uint8_t*>(buffer);
    const size_t frame_bytes = snd_pcm_frames_to_bytes(handle_.get(), 1);
    snd_pcm_uframes_t remaining = frames;
    int attempts = 0;
    while (remaining > 0) {
        const snd_pcm_sframes_t written = snd_pcm_writei(handle_.get(), data, remaining);
        if (written >= 0) {
            remaining -= static_cast<snd_pcm_uframes_t>(written);
            data += static_cast<size_t>(written) * frame_bytes;
            attempts = 0;
            continue;
        }
        if (++attempts > kMaxRecoverAttempts) {
            LOG(ERROR) << name_ << ": giving up after " << attempts << " failed writes";
            return written;
        }
        if (const int err = Recover(static_cast<int>(written)); err < 0) return err;
    }
    return static_cast<snd_pcm_sframes_t>(frames);
}

snd_pcm_sframes_t Pcm::Read(void* buffer, snd_pcm_uframes_t frames) {
    uint8_t* data = static_cast<uint8_t*>(buffer);
    const size_t frame_bytes = snd_pcm_frames_to_bytes(handle_.get(), 1);
    snd_pcm_uframes_t remaining = frames;
    int attempts = 0;
    while (remaining > 0) {
        const snd_pcm_sframes_t read = snd_pcm_readi(handle_.get(), data, remaining);
        if (read >= 0) {
            remaining -= static_cast<snd_pcm_uframes_t>(read);
            data += static_cast<size_t>(read) * frame_bytes;
            attempts = 0;
            continue;
        }
        if (++attempts > kMaxRecoverAttempts) {
            LOG(ERROR) << name_ << ": giving up after " << attempts << " failed reads";
            return read;
        }
        if (const int err = Recover(static_cast<int>(read)); err < 0) return err;
    }
    return static_cast<snd_pcm_sframes_t>(frames);
}

int Pcm::Prepare() {
    const int err = snd_pcm_prepare(handle_.get());
    if (err < 0) LOG(WARNING) << name_ << ": snd_pcm_prepare: " << ErrorString(err);
    return err;
}

int Pcm::Start() {
    const int err = snd_pcm_start(handle_.get());
    if (err < 0 && err != -EBADFD) {
        LOG(WARNING) << name_ << ": snd_pcm_start: " << ErrorString(err);
    }
    return err;
}

int Pcm::Drop() {
    const int err = snd_pcm_drop(handle_.get());
    if (err < 0) LOG(WARNING) << name_ << ": snd_pcm_drop: " << ErrorString(err);
    return err;
}

int Pcm::Drain() {
    const int err = snd_pcm_drain(handle_.get());
    if (err < 0 && err != -EBADFD) {
        LOG(WARNING) << name_ << ": snd_pcm_drain: " << ErrorString(err);
    }
    return err;
}

int Pcm::Pause(bool pause) {
    if (!can_pause_) return -ENOSYS;
    const int err = snd_pcm_pause(handle_.get(), pause ? 1 : 0);
    if (err < 0) LOG(WARNING) << name_ << ": snd_pcm_pause(" << pause << "): " << ErrorString(err);
    return err;
}

snd_pcm_state_t Pcm::State() const {
    return snd_pcm_state(handle_.get());
}

std::optional<PcmPosition> Pcm::QueryPosition() const {
    PcmStatusPtr status = AllocPcmStatus();
    if (const int err = snd_pcm_status(handle_.get(), status.get()); err < 0) {
        LOG(DEBUG) << name_ << ": snd_pcm_status: " << ErrorString(err);
        return std::nullopt;
    }
    PcmPosition position;
    position.delay_frames = snd_pcm_status_get_delay(status.get());
    snd_htimestamp_t ts = {};
    snd_pcm_status_get_htstamp(status.get(), &ts);
    position.time_ns = (ts.tv_sec == 0 && ts.tv_nsec == 0) ? NowMonotonicNs() : TimespecToNs(ts);
    const snd_pcm_state_t state = snd_pcm_status_get_state(status.get());
    if (state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_DRAINING) {
        // Not running: the delay reported by some drivers is garbage.
        if (position.delay_frames < 0) position.delay_frames = 0;
        position.time_ns = NowMonotonicNs();
    }
    return position;
}

int32_t Pcm::LatencyMs() const {
    snd_pcm_sframes_t delay = 0;
    if (const int err = snd_pcm_delay(handle_.get(), &delay); err < 0 || delay < 0) {
        // Fall back to the ring buffer size.
        delay = static_cast<snd_pcm_sframes_t>(config_.buffer_frames);
    }
    if (config_.rate == 0) return 0;
    return static_cast<int32_t>((delay * 1000LL) / config_.rate);
}

// --- Capability probing ------------------------------------------------------

std::optional<HwCapabilities> QueryCapabilities(const std::string& name, snd_pcm_stream_t stream) {
    snd_pcm_t* raw = nullptr;
    int err = snd_pcm_open(&raw, name.c_str(), stream, SND_PCM_NONBLOCK);
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_pcm_open(" << name << ", " << snd_pcm_stream_name(stream)
                     << "): " << ErrorString(err);
        return std::nullopt;
    }
    PcmHandle handle(raw);
    HwParamsPtr params = AllocHwParams();
    err = snd_pcm_hw_params_any(raw, params.get());
    if (err < 0) {
        LOG(WARNING) << __func__ << ": snd_pcm_hw_params_any(" << name << "): " << ErrorString(err);
        return std::nullopt;
    }

    HwCapabilities caps;
    for (int f = 0; f <= SND_PCM_FORMAT_LAST; ++f) {
        const auto format = static_cast<snd_pcm_format_t>(f);
        if (!ToAidlFormat(format).has_value()) continue;  // Only formats Android understands.
        if (snd_pcm_hw_params_test_format(raw, params.get(), format) == 0) {
            caps.formats.insert(format);
        }
    }
    for (const unsigned int rate : StandardSampleRates()) {
        if (snd_pcm_hw_params_test_rate(raw, params.get(), rate, 0) == 0) {
            caps.rates.insert(rate);
        }
    }
    snd_pcm_hw_params_get_channels_min(params.get(), &caps.min_channels);
    snd_pcm_hw_params_get_channels_max(params.get(), &caps.max_channels);
    LOG(INFO) << __func__ << ": " << name << " (" << snd_pcm_stream_name(stream)
              << "): " << caps.ToString();
    if (caps.IsEmpty()) return std::nullopt;
    return caps;
}

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
