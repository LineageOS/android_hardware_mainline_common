/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LegacyEffect_Factory"

#include "LegacyFactory.h"

#include <algorithm>
#include <sstream>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android/binder_ibinder_platform.h>
#include <media/AidlConversionCppNdk.h>
#include <media/AidlConversionEffect.h>
#include <system/audio_aidl_utils.h>
#include <system/thread_defs.h>

#include "LegacyEffect.h"
#include "params/ParameterTranslator.h"

// Defined by libaudioeffectaidlcommon (EffectImpl.cpp): resets, closes and
// validates an instance before it is dropped.
extern "C" binder_exception_t destroyEffect(
        const std::shared_ptr<aidl::android::hardware::audio::effect::IEffect>& instance);

namespace aidl::android::hardware::audio::effect::legacy {

using ::aidl::android::media::audio::common::AudioUuid;

namespace {

std::string Key(const AudioUuid& uuid) {
    return ::android::audio::utils::toString(uuid);
}

}  // namespace

LegacyFactory::LegacyFactory(const std::string& config_file) : config_(config_file) {
    LOG(INFO) << __func__ << ": configuration " << config_file << ", "
              << config_.getSkippedElements() << " element(s) skipped by the parser";
    LoadLibraries();
    LoadEffects();
    std::lock_guard guard(lock_);
    LOG(INFO) << __func__ << ": " << libraries_.size() << " librar(y/ies), " << effects_.size()
              << " effect(s) available";
}

// --- start-up ----------------------------------------------------------------

void LegacyFactory::LoadLibraries() {
    std::lock_guard guard(lock_);
    for (const auto& [name, path] : config_.getLibraryMap()) {
        if (auto library = LegacyLibrary::Open(name, path); library != nullptr) {
            libraries_[name] = std::move(library);
        } else {
            LOG(ERROR) << __func__ << ": library \"" << name << "\" (" << path
                       << ") is not usable, its effects will be missing";
        }
    }
}

std::optional<LegacyFactory::Entry> LegacyFactory::MakeEntry(
        const EffectConfig::Library& library, const std::optional<AudioUuid>& proxy) {
    std::shared_ptr<LegacyLibrary> legacy_library;
    {
        std::lock_guard guard(lock_);
        const auto it = libraries_.find(library.name);
        if (it == libraries_.end()) {
            LOG(WARNING) << __func__ << ": effect " << Key(library.uuid)
                         << " refers to unknown library \"" << library.name << "\"";
            return std::nullopt;
        }
        legacy_library = it->second;
    }

    const auto legacy_uuid = ::aidl::android::aidl2legacy_AudioUuid_audio_uuid_t(library.uuid);
    if (!legacy_uuid.ok()) return std::nullopt;
    const auto legacy_descriptor = legacy_library->GetDescriptor(legacy_uuid.value());
    if (!legacy_descriptor.has_value()) return std::nullopt;

    const auto descriptor =
            ::aidl::android::legacy2aidl_effect_descriptor_Descriptor(*legacy_descriptor);
    if (!descriptor.ok()) {
        LOG(WARNING) << __func__ << ": descriptor of " << legacy_descriptor->name
                     << " can not be converted";
        return std::nullopt;
    }

    Entry entry{.library = legacy_library,
                .legacy_uuid = legacy_uuid.value(),
                .descriptor = descriptor.value()};
    entry.descriptor.common.id.proxy = proxy;
    if (library.type.has_value() && library.type.value() != entry.descriptor.common.id.type) {
        LOG(WARNING) << __func__ << ": " << legacy_descriptor->name
                     << ": the configuration says type " << Key(library.type.value())
                     << " but the library reports " << Key(entry.descriptor.common.id.type)
                     << "; using the library's";
    }

    // Capabilities (ranges) can only be read from an instance.
    if (auto instance = legacy_library->CreateEffect(legacy_uuid.value(), 0 /*session*/, 0 /*io*/);
        instance != nullptr) {
        if (instance->CommandWithStatusReply(EFFECT_CMD_INIT, 0, nullptr) == 0) {
            CreateTranslator(entry.descriptor.common.id.type)
                    ->FillCapability(*instance, &entry.descriptor.capability);
        }
    } else {
        LOG(WARNING) << __func__ << ": " << legacy_descriptor->name
                     << " can not be instantiated for capability probing";
    }
    return entry;
}

void LegacyFactory::LoadEffects() {
    for (const auto& [name, libraries] : config_.getEffectsMap()) {
        std::optional<AudioUuid> proxy;
        if (libraries.proxyLibrary.has_value()) proxy = libraries.proxyLibrary->uuid;
        for (const auto& library : libraries.libraries) {
            auto entry = MakeEntry(library, proxy);
            if (!entry.has_value()) {
                LOG(WARNING) << __func__ << ": effect \"" << name << "\" (" << Key(library.uuid)
                             << ") skipped";
                continue;
            }
            LOG(INFO) << __func__ << ": effect \"" << name
                      << "\": " << entry->descriptor.common.name << " by "
                      << entry->descriptor.common.implementor << ", type "
                      << Key(entry->descriptor.common.id.type) << ", impl " << Key(library.uuid)
                      << (proxy.has_value() ? ", proxy " + Key(*proxy) : "") << ", flags "
                      << entry->descriptor.common.flags.toString();
            std::lock_guard guard(lock_);
            effects_[Key(library.uuid)] = std::move(*entry);
        }
    }
}

const LegacyFactory::Entry* LegacyFactory::FindEntry(const AudioUuid& uuid) const {
    const auto it = effects_.find(Key(uuid));
    return it != effects_.end() ? &it->second : nullptr;
}

// --- IFactory ----------------------------------------------------------------

ndk::ScopedAStatus LegacyFactory::queryEffects(const std::optional<AudioUuid>& type,
                                               const std::optional<AudioUuid>& implementation,
                                               const std::optional<AudioUuid>& proxy,
                                               std::vector<Descriptor>* descriptors) {
    std::lock_guard guard(lock_);
    for (const auto& [key, entry] : effects_) {
        const Descriptor::Identity& id = entry.descriptor.common.id;
        if (type.has_value() && *type != id.type) continue;
        if (implementation.has_value() && *implementation != id.uuid) continue;
        if (proxy.has_value() && (!id.proxy.has_value() || *proxy != *id.proxy)) continue;
        descriptors->push_back(entry.descriptor);
    }
    LOG(DEBUG) << __func__ << ": " << descriptors->size() << " descriptor(s)";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus LegacyFactory::queryProcessing(const std::optional<Processing::Type>& type,
                                                  std::vector<Processing>* processings) {
    std::lock_guard guard(lock_);
    for (const auto& [processing_type, effect_libraries] : config_.getProcessingMap()) {
        if (type.has_value() && *type != processing_type) continue;
        Processing processing;
        processing.type = processing_type;
        for (const auto& libraries : effect_libraries) {
            for (const auto& library : libraries.libraries) {
                if (const Entry* entry = FindEntry(library.uuid); entry != nullptr) {
                    processing.ids.push_back(entry->descriptor);
                } else {
                    LOG(WARNING) << __func__ << ": processing chain refers to unavailable effect "
                                 << Key(library.uuid);
                }
            }
        }
        processings->push_back(std::move(processing));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus LegacyFactory::createEffect(const AudioUuid& implementation,
                                               std::shared_ptr<IEffect>* effect) {
    std::lock_guard guard(lock_);
    const Entry* entry = FindEntry(implementation);
    if (entry == nullptr) {
        LOG(ERROR) << __func__ << ": no effect with implementation UUID " << Key(implementation);
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::shared_ptr<LegacyEffect> instance = ndk::SharedRefBase::make<LegacyEffect>(
            entry->library, entry->legacy_uuid, entry->descriptor);
    ndk::SpAIBinder binder = instance->asBinder();
    AIBinder_setMinSchedulerPolicy(binder.get(), SCHED_NORMAL, ANDROID_PRIORITY_AUDIO);
    AIBinder_setInheritRt(binder.get(), true);

    // Forget instances that went away without destroyEffect().
    std::erase_if(instances_, [](const std::weak_ptr<IEffect>& weak) { return weak.expired(); });
    instances_.push_back(instance);
    *effect = instance;
    LOG(DEBUG) << __func__ << ": " << entry->descriptor.common.name << ", " << instances_.size()
               << " live instance(s)";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus LegacyFactory::destroyEffect(const std::shared_ptr<IEffect>& effect) {
    std::lock_guard guard(lock_);
    const auto it = std::find_if(
            instances_.begin(), instances_.end(),
            [&effect](const std::weak_ptr<IEffect>& weak) { return weak.lock() == effect; });
    if (it == instances_.end()) {
        LOG(ERROR) << __func__ << ": unknown effect instance " << effect.get();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    instances_.erase(it);
    // Shared with the example effects: RESET + close (or refuse when an old
    // client still expects the INIT state).
    if (const binder_exception_t status = ::destroyEffect(effect); status != EX_NONE) {
        return ndk::ScopedAStatus::fromExceptionCode(status);
    }
    return ndk::ScopedAStatus::ok();
}

binder_status_t LegacyFactory::dump(int fd, const char** /*args*/, uint32_t /*num_args*/) {
    std::lock_guard guard(lock_);
    std::ostringstream os;
    os << "Legacy effect factory\n";
    os << "Libraries (" << libraries_.size() << "):\n";
    for (const auto& [name, library] : libraries_) {
        os << "  " << name << " -> " << library->path() << "\n";
    }
    os << "Effects (" << effects_.size() << "):\n";
    for (const auto& [key, entry] : effects_) {
        os << "  " << entry.descriptor.common.name << " (" << entry.descriptor.common.implementor
           << ") impl " << key << " type " << Key(entry.descriptor.common.id.type) << " lib "
           << entry.library->name() << "\n";
    }
    os << "Live instances: "
       << std::count_if(instances_.begin(), instances_.end(),
                        [](const std::weak_ptr<IEffect>& weak) { return !weak.expired(); })
       << "\n";
    ::android::base::WriteStringToFd(os.str(), fd);
    return STATUS_OK;
}

}  // namespace aidl::android::hardware::audio::effect::legacy
