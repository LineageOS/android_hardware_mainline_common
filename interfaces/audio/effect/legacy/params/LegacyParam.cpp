/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Param"

#include "params/LegacyParam.h"

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::effect::legacy {

LegacyParam::LegacyParam(uint32_t psize, uint32_t vsize)
    : bytes_(sizeof(effect_param_t) + Padded(psize) + vsize, 0) {
    param()->psize = psize;
    param()->vsize = vsize;
}

LegacyParam::LegacyParam(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < sizeof(effect_param_t)) {
        LOG(ERROR) << __func__ << ": " << bytes.size() << " bytes is too small for effect_param_t";
        return;
    }
    const auto* header = reinterpret_cast<const effect_param_t*>(bytes.data());
    const size_t needed = sizeof(effect_param_t) + Padded(header->psize) + header->vsize;
    if (header->psize > EFFECT_PARAM_SIZE_MAX || header->vsize > EFFECT_PARAM_SIZE_MAX ||
        bytes.size() < needed) {
        LOG(ERROR) << __func__ << ": inconsistent effect_param_t: psize=" << header->psize
                   << " vsize=" << header->vsize << " buffer=" << bytes.size();
        return;
    }
    bytes_.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(needed));
}

int32_t SetParam(LegacyEffectHandle& effect, LegacyParam& param) {
    if (!param.IsValid()) return -EINVAL;
    param.param()->status = 0;
    const int32_t status =
            effect.CommandWithStatusReply(EFFECT_CMD_SET_PARAM, param.size(), param.param());
    if (status != 0) {
        LOG(DEBUG) << __func__ << ": " << effect.descriptor().name
                   << " SET_PARAM psize=" << param.psize() << " vsize=" << param.vsize()
                   << " failed: " << status;
    }
    return status;
}

int32_t GetParam(LegacyEffectHandle& effect, LegacyParam* param) {
    if (!param->IsValid()) return -EINVAL;
    param->param()->status = 0;
    // Legacy effects write the reply into a buffer of the same shape as the
    // request; the request buffer already has room for the value.
    LegacyParam reply = *param;
    uint32_t reply_size = reply.size();
    const int32_t status = effect.Command(EFFECT_CMD_GET_PARAM, param->size(), param->param(),
                                          &reply_size, reply.param());
    if (status != 0) {
        LOG(DEBUG) << __func__ << ": " << effect.descriptor().name
                   << " GET_PARAM psize=" << param->psize() << " failed: " << status;
        return status;
    }
    if (reply.param()->status != 0) {
        LOG(DEBUG) << __func__ << ": " << effect.descriptor().name << " GET_PARAM replied status "
                   << reply.param()->status;
        return reply.param()->status;
    }
    if (reply.param()->vsize > param->vsize()) {
        LOG(WARNING) << __func__ << ": " << effect.descriptor().name << " GET_PARAM returned vsize "
                     << reply.param()->vsize << " > requested " << param->vsize();
        reply.param()->vsize = param->vsize();
    }
    *param = std::move(reply);
    return 0;
}

}  // namespace aidl::android::hardware::audio::effect::legacy
