/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Context"

#include "LegacyEffectContext.h"

#include <algorithm>
#include <cstring>

#include <Utils.h>
#include <android-base/logging.h>
#include <audio_utils/primitives.h>
#include <media/AidlConversionCppNdk.h>
#include <system/audio_effect.h>

namespace aidl::android::hardware::audio::effect::legacy {

using ::aidl::android::hardware::audio::common::getChannelCount;
using ::aidl::android::media::audio::common::AudioDeviceDescription;
using ::aidl::android::media::audio::common::AudioMode;
using ::aidl::android::media::audio::common::AudioSource;

namespace {

// Volume commands use unsigned 8.24 fixed point.
constexpr float kUnityGain = static_cast<float>(1 << 24);

uint32_t ToFixedGain(float volume) {
    return static_cast<uint32_t>(std::clamp(volume, 0.0f, 1.0f) * kUnityGain);
}

float FromFixedGain(uint32_t gain) {
    return static_cast<float>(gain) / kUnityGain;
}

// The largest burst handed to the effect in one process() call.
constexpr size_t kMaxProcessFrames = 16384;

// A legacy configuration carries one frame count per buffer, but some effects
// insist that both are equal. The count is only an upper bound of what
// process() gets, so use the larger one for both.
size_t FrameCount(const Parameter::Common& common) {
    return static_cast<size_t>(std::max(common.input.frameCount, common.output.frameCount));
}

}  // namespace

LegacyEffectContext::LegacyEffectContext(size_t status_depth, const Parameter::Common& common,
                                         std::unique_ptr<LegacyEffectHandle> handle, bool is_input)
    : EffectContext(status_depth, common),
      handle_(std::move(handle)),
      is_input_(is_input),
      volume_control_((handle_->descriptor().flags & EFFECT_FLAG_VOLUME_MASK) ==
                      EFFECT_FLAG_VOLUME_CTRL),
      auxiliary_((handle_->descriptor().flags & EFFECT_FLAG_TYPE_MASK) ==
                 EFFECT_FLAG_TYPE_AUXILIARY) {}

bool LegacyEffectContext::Initialize() {
    if (const int32_t status = handle_->CommandWithStatusReply(EFFECT_CMD_INIT, 0, nullptr);
        status != 0) {
        LOG(ERROR) << __func__ << ": " << handle_->descriptor().name
                   << " EFFECT_CMD_INIT failed: " << status;
        return false;
    }
    return ApplyConfig(mCommon) == RetCode::SUCCESS;
}

// --- configuration -----------------------------------------------------------

int32_t LegacyEffectContext::SendConfig(const Parameter::Common& common, audio_format_t format,
                                        bool mono_input) {
    const size_t frame_count = FrameCount(common);
    auto fill = [&](const ::aidl::android::media::audio::common::AudioConfig& aidl,
                    buffer_config_t* legacy, uint8_t access_mode, bool mono) -> bool {
        const auto mask = ::aidl::android::aidl2legacy_AudioChannelLayout_audio_channel_mask_t(
                aidl.base.channelMask, is_input_);
        if (!mask.ok()) {
            LOG(ERROR) << __func__ << ": unsupported channel mask "
                       << aidl.base.channelMask.toString();
            return false;
        }
        *legacy = {};
        legacy->buffer.frameCount = frame_count;
        legacy->samplingRate = static_cast<uint32_t>(aidl.base.sampleRate);
        legacy->channels = mono ? static_cast<uint32_t>(AUDIO_CHANNEL_OUT_MONO)
                                : static_cast<uint32_t>(mask.value());
        legacy->format = static_cast<uint8_t>(format);
        legacy->accessMode = access_mode;
        legacy->mask = EFFECT_CONFIG_ALL;
        return true;
    };
    effect_config_t config;
    if (!fill(common.input, &config.inputCfg, EFFECT_BUFFER_ACCESS_READ, mono_input) ||
        !fill(common.output, &config.outputCfg, EFFECT_BUFFER_ACCESS_WRITE, false)) {
        return -EINVAL;
    }
    return handle_->CommandWithStatusReply(EFFECT_CMD_SET_CONFIG, sizeof(config), &config);
}

RetCode LegacyEffectContext::ApplyConfig(const Parameter::Common& common) {
    // An auxiliary effect takes a mono send, which AudioFlinger used to mix
    // down before configuring it; some (the reverbs) refuse anything else.
    const bool mono_input = auxiliary_ && getChannelCount(common.input.base.channelMask) > 1;
    // The AIDL side always speaks 32-bit float; most legacy effects do too.
    if (SendConfig(common, AUDIO_FORMAT_PCM_FLOAT, mono_input) == 0) {
        int16_mode_ = false;
    } else if (SendConfig(common, AUDIO_FORMAT_PCM_16_BIT, mono_input) == 0) {
        LOG(INFO) << __func__ << ": " << handle_->descriptor().name
                  << " does not accept float, converting to 16-bit PCM";
        int16_mode_ = true;
    } else {
        LOG(ERROR) << __func__ << ": " << handle_->descriptor().name
                   << " rejected the configuration " << common.toString();
        return RetCode::ERROR_ILLEGAL_PARAMETER;
    }
    downmix_input_ = mono_input;
    const size_t frame_count = FrameCount(common);
    const size_t in_samples = frame_count * getChannelCount(common.input.base.channelMask);
    const size_t out_samples = frame_count * getChannelCount(common.output.base.channelMask);
    in16_.resize(in_samples);
    mono_scratch_.resize(mono_input ? frame_count : 0);
    out16_.resize(out_samples);
    out_scratch_.resize(out_samples);
    LOG(DEBUG) << __func__ << ": " << handle_->descriptor().name << " configured, "
               << common.input.base.sampleRate << " Hz, in " << in_samples << " / out "
               << out_samples << " samples per burst" << (int16_mode_ ? ", 16-bit" : "");
    return RetCode::SUCCESS;
}

RetCode LegacyEffectContext::setCommon(const Parameter::Common& common) {
    // The base constructor calls this before handle_ exists; Initialize()
    // applies the configuration in that case.
    if (const RetCode ret = EffectContext::setCommon(common); ret != RetCode::SUCCESS) return ret;
    if (handle_ == nullptr) return RetCode::SUCCESS;
    return ApplyConfig(common);
}

// --- indications -------------------------------------------------------------

RetCode LegacyEffectContext::setOutputDevice(const std::vector<AudioDeviceDescription>& devices) {
    if (const RetCode ret = EffectContext::setOutputDevice(devices); ret != RetCode::SUCCESS) {
        return ret;
    }
    uint32_t legacy_devices = 0;
    for (const auto& device : devices) {
        const auto legacy =
                ::aidl::android::aidl2legacy_AudioDeviceDescription_audio_devices_t(device);
        if (legacy.ok()) legacy_devices |= static_cast<uint32_t>(legacy.value());
    }
    const uint32_t cmd = is_input_ ? EFFECT_CMD_SET_INPUT_DEVICE : EFFECT_CMD_SET_DEVICE;
    uint32_t reply_size = 0;
    // Indication only: the legacy API defines no reply for this command.
    handle_->Command(cmd, sizeof(legacy_devices), &legacy_devices, &reply_size, nullptr);
    return RetCode::SUCCESS;
}

RetCode LegacyEffectContext::setAudioMode(const AudioMode& mode) {
    if (const RetCode ret = EffectContext::setAudioMode(mode); ret != RetCode::SUCCESS) return ret;
    const auto legacy = ::aidl::android::aidl2legacy_AudioMode_audio_mode_t(mode);
    if (!legacy.ok()) return RetCode::ERROR_ILLEGAL_PARAMETER;
    uint32_t value = static_cast<uint32_t>(legacy.value());
    uint32_t reply_size = 0;
    handle_->Command(EFFECT_CMD_SET_AUDIO_MODE, sizeof(value), &value, &reply_size, nullptr);
    return RetCode::SUCCESS;
}

RetCode LegacyEffectContext::setAudioSource(const AudioSource& source) {
    if (const RetCode ret = EffectContext::setAudioSource(source); ret != RetCode::SUCCESS) {
        return ret;
    }
    const auto legacy = ::aidl::android::aidl2legacy_AudioSource_audio_source_t(source);
    if (!legacy.ok()) return RetCode::ERROR_ILLEGAL_PARAMETER;
    uint32_t value = static_cast<uint32_t>(legacy.value());
    uint32_t reply_size = 0;
    handle_->Command(EFFECT_CMD_SET_AUDIO_SOURCE, sizeof(value), &value, &reply_size, nullptr);
    return RetCode::SUCCESS;
}

RetCode LegacyEffectContext::setVolumeStereo(const Parameter::VolumeStereo& volume) {
    uint32_t request[2] = {ToFixedGain(volume.left), ToFixedGain(volume.right)};
    uint32_t reply[2] = {request[0], request[1]};
    uint32_t reply_size = volume_control_ ? sizeof(reply) : 0;
    const int32_t status = handle_->Command(EFFECT_CMD_SET_VOLUME, sizeof(request), request,
                                            &reply_size, volume_control_ ? reply : nullptr);
    Parameter::VolumeStereo applied = volume;
    if (status == 0 && volume_control_ && reply_size == sizeof(reply)) {
        // A volume controlling effect tells what is left for the framework
        // to apply; report that back through getVolumeStereo().
        applied.left = FromFixedGain(reply[0]);
        applied.right = FromFixedGain(reply[1]);
    }
    return EffectContext::setVolumeStereo(applied);
}

// --- state -------------------------------------------------------------------

RetCode LegacyEffectContext::enable() {
    if (const int32_t status = handle_->CommandWithStatusReply(EFFECT_CMD_ENABLE, 0, nullptr);
        status != 0) {
        LOG(ERROR) << __func__ << ": " << handle_->descriptor().name << " failed: " << status;
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }
    return RetCode::SUCCESS;
}

RetCode LegacyEffectContext::disable() {
    if (const int32_t status = handle_->CommandWithStatusReply(EFFECT_CMD_DISABLE, 0, nullptr);
        status != 0) {
        LOG(WARNING) << __func__ << ": " << handle_->descriptor().name << " failed: " << status;
        return RetCode::ERROR_EFFECT_LIB_ERROR;
    }
    return RetCode::SUCCESS;
}

RetCode LegacyEffectContext::reset() {
    uint32_t reply_size = 0;
    handle_->Command(EFFECT_CMD_RESET, 0, nullptr, &reply_size, nullptr);
    return RetCode::SUCCESS;
}

// --- processing --------------------------------------------------------------

int32_t LegacyEffectContext::ProcessChunk(float* in, float* out, const float* src, size_t frames,
                                          size_t in_channels, size_t out_channels) {
    const size_t in_samples = frames * in_channels;
    const size_t out_samples = frames * out_channels;
    const size_t src_samples = frames * (downmix_input_ ? 1 : in_channels);
    audio_buffer_t in_buffer = {};
    audio_buffer_t out_buffer = {};
    in_buffer.frameCount = frames;
    out_buffer.frameCount = frames;
    int32_t status;
    if (handle_->bypass()) {
        // Treated like an effect with nothing to add, see below.
        status = -ENODATA;
    } else if (int16_mode_) {
        memcpy_to_i16_from_float(in16_.data(), src, src_samples);
        in_buffer.s16 = in16_.data();
        out_buffer.s16 = out16_.data();
        status = handle_->Process(&in_buffer, &out_buffer);
        if (status == 0) {
            memcpy_to_float_from_i16(out, out16_.data(), out_samples);
        }
    } else if (in_channels == out_channels && !downmix_input_) {
        // In place, like the legacy framework did for insert effects.
        in_buffer.f32 = in;
        out_buffer.f32 = out;
        status = handle_->Process(&in_buffer, &out_buffer);
    } else {
        in_buffer.f32 = const_cast<float*>(src);
        out_buffer.f32 = out_scratch_.data();
        status = handle_->Process(&in_buffer, &out_buffer);
        if (status == 0) {
            std::memcpy(out, out_scratch_.data(), out_samples * sizeof(float));
        }
    }

    if (status == -ENODATA) {
        // The effect has nothing to add (typically: not enabled, or bypassed).
        // An auxiliary effect outputs only its own contribution, which is none.
        // Any other passes the input through so that the chain keeps flowing.
        if (auxiliary_) {
            std::memset(out, 0, out_samples * sizeof(float));
        } else if (in != out) {
            std::memcpy(out, in, std::min(in_samples, out_samples) * sizeof(float));
            if (out_samples > in_samples) {
                std::memset(out + in_samples, 0, (out_samples - in_samples) * sizeof(float));
            }
        }
        status = 0;
    }
    return status;
}

IEffect::Status LegacyEffectContext::Process(float* in, float* out, int samples) {
    const size_t in_channels = mInputFrameSize / sizeof(float);
    const size_t out_channels = mOutputFrameSize / sizeof(float);
    if (in_channels == 0 || out_channels == 0 || samples <= 0) {
        return {EX_ILLEGAL_STATE, 0, 0};
    }
    const size_t frames = static_cast<size_t>(samples) / in_channels;
    const size_t in_samples = frames * in_channels;
    const size_t out_samples = frames * out_channels;
    if (out_samples > out_scratch_.size()) {
        return {EX_ILLEGAL_STATE, 0, 0};
    }

    // The effect takes a mono send: hand it the mean of the channels.
    const float* src = in;
    if (downmix_input_) {
        for (size_t i = 0; i < frames; ++i) {
            float sum = 0.0f;
            for (size_t c = 0; c < in_channels; ++c) sum += in[i * in_channels + c];
            mono_scratch_[i] = sum / static_cast<float>(in_channels);
        }
        src = mono_scratch_.data();
    }

    // Some effects (the bundle) misbehave with a burst of 32768 frames or more
    // and output silence. Hand them bounded chunks.
    const size_t src_channels = downmix_input_ ? 1 : in_channels;
    for (size_t done = 0; done < frames;) {
        const size_t chunk = std::min(kMaxProcessFrames, frames - done);
        if (const int32_t status =
                    ProcessChunk(in + done * in_channels, out + done * out_channels,
                                 src + done * src_channels, chunk, in_channels, out_channels);
            status != 0) {
            LOG(ERROR) << __func__ << ": " << handle_->descriptor().name
                       << " process failed: " << status;
            return {EX_ILLEGAL_STATE, 0, 0};
        }
        done += chunk;
    }
    return {STATUS_OK, static_cast<int32_t>(in_samples), static_cast<int32_t>(out_samples)};
}

}  // namespace aidl::android::hardware::audio::effect::legacy
