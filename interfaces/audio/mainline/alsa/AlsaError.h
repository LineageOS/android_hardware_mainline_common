/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <string>

#include <alsa/asoundlib.h>

namespace aidl::android::hardware::audio::core::mainline::alsa {

// Formats a negative alsa-lib / errno return code for logging.
inline std::string ErrorString(int err) {
    return std::string(snd_strerror(err)) + " (" + std::to_string(err) + ")";
}

// RAII owners for alsa-lib handles. The deleters tolerate nullptr so that a
// default constructed owner is valid.

struct CtlCloser {
    void operator()(snd_ctl_t* ctl) const {
        if (ctl != nullptr) snd_ctl_close(ctl);
    }
};
using CtlHandle = std::unique_ptr<snd_ctl_t, CtlCloser>;

struct PcmCloser {
    void operator()(snd_pcm_t* pcm) const {
        if (pcm != nullptr) snd_pcm_close(pcm);
    }
};
using PcmHandle = std::unique_ptr<snd_pcm_t, PcmCloser>;

struct MixerCloser {
    void operator()(snd_mixer_t* mixer) const {
        if (mixer != nullptr) snd_mixer_close(mixer);
    }
};
using MixerHandle = std::unique_ptr<snd_mixer_t, MixerCloser>;

struct CardInfoFreer {
    void operator()(snd_ctl_card_info_t* info) const {
        if (info != nullptr) snd_ctl_card_info_free(info);
    }
};
using CardInfoPtr = std::unique_ptr<snd_ctl_card_info_t, CardInfoFreer>;

struct PcmInfoFreer {
    void operator()(snd_pcm_info_t* info) const {
        if (info != nullptr) snd_pcm_info_free(info);
    }
};
using PcmInfoPtr = std::unique_ptr<snd_pcm_info_t, PcmInfoFreer>;

struct HwParamsFreer {
    void operator()(snd_pcm_hw_params_t* params) const {
        if (params != nullptr) snd_pcm_hw_params_free(params);
    }
};
using HwParamsPtr = std::unique_ptr<snd_pcm_hw_params_t, HwParamsFreer>;

struct SwParamsFreer {
    void operator()(snd_pcm_sw_params_t* params) const {
        if (params != nullptr) snd_pcm_sw_params_free(params);
    }
};
using SwParamsPtr = std::unique_ptr<snd_pcm_sw_params_t, SwParamsFreer>;

struct PcmStatusFreer {
    void operator()(snd_pcm_status_t* status) const {
        if (status != nullptr) snd_pcm_status_free(status);
    }
};
using PcmStatusPtr = std::unique_ptr<snd_pcm_status_t, PcmStatusFreer>;

inline CardInfoPtr AllocCardInfo() {
    snd_ctl_card_info_t* info = nullptr;
    snd_ctl_card_info_malloc(&info);
    return CardInfoPtr(info);
}

inline PcmInfoPtr AllocPcmInfo() {
    snd_pcm_info_t* info = nullptr;
    snd_pcm_info_malloc(&info);
    return PcmInfoPtr(info);
}

inline HwParamsPtr AllocHwParams() {
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_malloc(&params);
    return HwParamsPtr(params);
}

inline SwParamsPtr AllocSwParams() {
    snd_pcm_sw_params_t* params = nullptr;
    snd_pcm_sw_params_malloc(&params);
    return SwParamsPtr(params);
}

inline PcmStatusPtr AllocPcmStatus() {
    snd_pcm_status_t* status = nullptr;
    snd_pcm_status_malloc(&status);
    return PcmStatusPtr(status);
}

// Redirects alsa-lib's internal error messages to logcat. Call once at
// start-up.
void InstallAlsaErrorHandler();

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
