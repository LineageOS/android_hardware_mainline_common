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
// How many hardware rates to try when the device rejects the requested one.
constexpr size_t kMaxSlaveRateAttempts = 4;
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
    LOG(DEBUG) << __func__ << ": Begin";
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
        // We only ever transfer with snd_pcm_writei / readi.
        LOG(WARNING) << __func__ << ": set_access(RW_INTERLEAVED): " << ErrorString(err);
        return err;
    }
    err = snd_pcm_hw_params_set_format(pcm, params.get(), config.format);
    if (err < 0) {
        LOG(DEBUG) << __func__ << ": set_format(" << snd_pcm_format_name(config.format)
                   << "): " << ErrorString(err);
        if (strict) return err;
    }
    err = snd_pcm_hw_params_set_channels(pcm, params.get(), config.channels);
    if (err < 0) {
        LOG(DEBUG) << __func__ << ": set_channels(" << config.channels << "): " << ErrorString(err);
        if (strict) return err;
    }
    unsigned int rate = config.rate;
    err = snd_pcm_hw_params_set_rate_near(pcm, params.get(), &rate, nullptr);
    if (err < 0) {
        LOG(DEBUG) << __func__ << ": set_rate_near(" << config.rate << "): " << ErrorString(err);
        if (strict) return err;
    }
    if (strict && rate != config.rate) {
        // Without the plug layer nothing would convert: refuse instead of
        // playing back at the wrong speed.
        LOG(DEBUG) << __func__ << ": rate " << config.rate << " not supported, closest is " << rate;
        return -EINVAL;
    }

    // The period is negotiated first and the buffer is then expressed as a
    // number of periods. Drivers constrain the period size (the Qualcomm
    // q6asm front-end for instance only accepts multiples of 480 frames) and
    // require an integer number of periods, so a buffer size that is chosen
    // independently of the granted period can end up not being a multiple of
    // it, which makes snd_pcm_hw_params() fail.
    snd_pcm_uframes_t period = config.period_frames;
    if (period > 0) {
        err = snd_pcm_hw_params_set_period_size_near(pcm, params.get(), &period, nullptr);
        if (err < 0) {
            LOG(WARNING) << __func__ << ": set_period_size_near(" << config.period_frames
                         << "): " << ErrorString(err);
        }
    }
    if (snd_pcm_hw_params_get_period_size(params.get(), &period, nullptr) < 0) period = 0;
    if (config.buffer_frames > 0 && period > 0) {
        unsigned int periods =
                static_cast<unsigned int>((config.buffer_frames + period / 2) / period);
        if (periods < 2) periods = 2;
        err = snd_pcm_hw_params_set_periods_near(pcm, params.get(), &periods, nullptr);
        if (err < 0) {
            LOG(WARNING) << __func__ << ": set_periods_near(" << periods
                         << "): " << ErrorString(err);
        }
    } else if (config.buffer_frames > 0) {
        snd_pcm_uframes_t buffer = config.buffer_frames;
        err = snd_pcm_hw_params_set_buffer_size_near(pcm, params.get(), &buffer);
        if (err < 0) {
            LOG(WARNING) << __func__ << ": set_buffer_size_near(" << config.buffer_frames
                         << "): " << ErrorString(err);
        }
    }

    err = snd_pcm_hw_params(pcm, params.get());
    if (err < 0) {
        // On a DPCM card this is where a back-end constraint that is invisible
        // to the front-end (and therefore to hw_params_any / test_rate) shows
        // up, typically as -EINVAL for an unsupported rate.
        LOG(WARNING) << __func__ << ": snd_pcm_hw_params({" << config.ToString()
                     << "}): " << ErrorString(err);
        return err;
    }

    // Report back what the driver actually granted, never what we asked for.
    *effective = config;
    if (snd_pcm_format_t format; snd_pcm_hw_params_get_format(params.get(), &format) == 0) {
        effective->format = format;
    }
    if (unsigned int channels = 0; snd_pcm_hw_params_get_channels(params.get(), &channels) == 0) {
        effective->channels = channels;
    }
    if (unsigned int granted_rate = 0;
        snd_pcm_hw_params_get_rate(params.get(), &granted_rate, nullptr) == 0) {
        effective->rate = granted_rate;
    } else {
        effective->rate = rate;
    }
    snd_pcm_hw_params_get_period_size(params.get(), &effective->period_frames, nullptr);
    snd_pcm_hw_params_get_buffer_size(params.get(), &effective->buffer_frames);
    *can_pause = snd_pcm_hw_params_can_pause(params.get()) != 0;
    LOG(DEBUG) << __func__ << ": Finish";
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

}  // namespace

// --- PcmConfig ---------------------------------------------------------------

std::string PcmConfig::ToString() const {
    std::ostringstream os;
    os << snd_pcm_format_name(format) << " " << channels << "ch " << rate
       << "Hz period=" << period_frames << " buffer=" << buffer_frames;
    return os.str();
}

// --- Pcm ---------------------------------------------------------------------

std::pair<std::string, bool> PlugName(const std::string& hw_name) {
    if (::android::base::StartsWith(hw_name, "hw:")) {
        return {"plughw:" + hw_name.substr(3), true};
    }
    return {hw_name, false};
}

namespace {

std::string SlaveRatePlugName(const std::string& hw_name, unsigned int slave_rate) {
    // "plughw:" derives the hardware rate from what the device reports as
    // supported, which is exactly the information that is wrong on a DPCM
    // card. Pin the hardware rate instead and let the plug layer convert
    // between it and the rate the stream was opened with.
    std::ostringstream os;
    os << "plug:{SLAVE={pcm \"" << hw_name << "\" rate " << slave_rate << "}}";
    return os.str();
}

// Hardware rates to try when the device rejects the requested one, most
// likely to be supported first. 48 kHz is the rate essentially every codec
// implements, and every candidate is a rate the plug layer can convert to.
std::vector<unsigned int> SlaveRateCandidates(unsigned int rate) {
    static constexpr unsigned int kPreferred[] = {48000,  96000, 192000, 88200, 44100,
                                                  176400, 32000, 24000,  16000, 8000};
    std::vector<unsigned int> candidates;
    for (const unsigned int candidate : kPreferred) {
        if (candidate == rate) continue;
        candidates.push_back(candidate);
        if (candidates.size() >= kMaxSlaveRateAttempts) break;
    }
    return candidates;
}

}  // namespace

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
    const auto [plug_name, is_plug] = PlugName(name);
    if (!is_plug) {
        LOG(ERROR) << __func__ << ": failed to open " << name << " with {" << config.ToString()
                   << "} and it is not a hw: device, giving up";
        return nullptr;
    }

    // A device can accept the configuration in hw_params_any / test_rate and
    // still reject it in hw_params: on a DPCM card (all the Qualcomm QDSP6
    // ones) the front-end PCM answers those queries on its own, while the
    // constraints of the back-end it is routed to are only applied when the
    // configuration is committed. The Qualcomm internal codec for example
    // only does 8 / 16 / 32 / 48 kHz and fails hw_params with -EINVAL for
    // everything else, even though the front-end announces 8 kHz - 192 kHz.
    // Plain "plughw:" cannot help there, because it picks the hardware rate
    // from the same optimistic answers; the rate has to be pinned explicitly.
    for (const unsigned int slave_rate : SlaveRateCandidates(config.rate)) {
        const std::string slave_name = SlaveRatePlugName(name, slave_rate);
        LOG(INFO) << __func__ << ": " << name << " does not accept {" << config.ToString()
                  << "} natively, retrying with the hardware at " << slave_rate << " Hz";
        if (auto pcm = TryOpen(slave_name, stream, config, true /*is_plug*/); pcm != nullptr) {
            LOG(INFO) << __func__ << ": " << name << " runs at " << slave_rate
                      << " Hz, converting from " << config.rate << " Hz in the plug layer";
            return pcm;
        }
    }

    LOG(INFO) << __func__ << ": " << name << " does not accept {" << config.ToString()
              << "} at any hardware rate, retrying through " << plug_name;
    auto pcm = TryOpen(plug_name, stream, config, true /*is_plug*/);
    if (pcm == nullptr) {
        LOG(ERROR) << __func__ << ": failed to open " << plug_name << " as well";
    }
    return pcm;
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
