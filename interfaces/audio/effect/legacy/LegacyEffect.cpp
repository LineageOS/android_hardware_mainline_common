/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Effect"

#include "LegacyEffect.h"

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::effect::legacy {

LegacyEffect::LegacyEffect(std::shared_ptr<LegacyLibrary> library, const effect_uuid_t& uuid,
                           Descriptor descriptor)
    : library_(std::move(library)),
      uuid_(uuid),
      descriptor_(std::move(descriptor)),
      translator_(CreateTranslator(descriptor_.common.id.type)) {
    LOG(DEBUG) << __func__ << ": " << descriptor_.common.name << " (" << UuidToString(uuid_) << ")";
}

LegacyEffect::~LegacyEffect() {
    // Stops the worker and releases the context (and with it the legacy
    // instance) when the client forgot to close.
    cleanUp();
    LOG(DEBUG) << __func__ << ": " << descriptor_.common.name;
}

bool LegacyEffect::IsInputEffect() const {
    return descriptor_.common.flags.type == Flags::Type::PRE_PROC;
}

ndk::ScopedAStatus LegacyEffect::getDescriptor(Descriptor* descriptor) {
    *descriptor = descriptor_;
    return ndk::ScopedAStatus::ok();
}

std::shared_ptr<EffectContext> LegacyEffect::createContext(const Parameter::Common& common) {
    if (context_ != nullptr) {
        LOG(DEBUG) << __func__ << ": " << descriptor_.common.name << " reusing context";
        return context_;
    }
    std::unique_ptr<LegacyEffectHandle> handle =
            library_->CreateEffect(uuid_, common.session, common.ioHandle);
    if (handle == nullptr) {
        LOG(ERROR) << __func__ << ": " << descriptor_.common.name
                   << " could not create the legacy instance";
        return nullptr;
    }
    auto context = std::make_shared<LegacyEffectContext>(1 /*status_depth*/, common,
                                                         std::move(handle), IsInputEffect());
    if (!context->Initialize()) {
        LOG(ERROR) << __func__ << ": " << descriptor_.common.name
                   << " could not initialise the legacy instance";
        return nullptr;
    }
    context_ = context;
    LOG(INFO) << __func__ << ": " << descriptor_.common.name << " opened, session "
              << common.session << " io " << common.ioHandle
              << (context->int16_mode() ? " (16-bit)" : "");
    return context;
}

RetCode LegacyEffect::releaseContext() {
    if (context_ != nullptr) {
        LOG(INFO) << __func__ << ": " << descriptor_.common.name << " closed";
        context_.reset();
    }
    return RetCode::SUCCESS;
}

ndk::ScopedAStatus LegacyEffect::setParameterSpecific(const Parameter::Specific& specific) {
    RETURN_IF(context_ == nullptr, EX_NULL_POINTER, "nullContext");
    return translator_->Set(context_->handle(), specific);
}

ndk::ScopedAStatus LegacyEffect::getParameterSpecific(const Parameter::Id& id,
                                                      Parameter::Specific* specific) {
    RETURN_IF(context_ == nullptr, EX_NULL_POINTER, "nullContext");
    return translator_->Get(context_->handle(), id, specific);
}

IEffect::Status LegacyEffect::effectProcessImpl(float* in, float* out, int samples) {
    if (context_ == nullptr) return {EX_NULL_POINTER, 0, 0};
    return context_->Process(in, out, samples);
}

}  // namespace aidl::android::hardware::audio::effect::legacy
