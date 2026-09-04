/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <aidl/android/hardware/audio/effect/BnFactory.h>
#include <android-base/thread_annotations.h>
#include <effectFactory-impl/EffectConfig.h>

#include "LegacyLibrary.h"

namespace aidl::android::hardware::audio::effect::legacy {

// IFactory serving legacy (hardware/audio_effect.h) effect libraries. The
// library list, the effect UUIDs and the pre/post-processing chains come from
// the vendor's audio_effects.xml, parsed with the example HAL's EffectConfig
// (the file format did not change between the legacy and the AIDL effect
// HALs). Effect type UUIDs, names and flags come from the libraries' own
// descriptors, so no name mapping is needed and vendor specific types work.
class LegacyFactory final : public BnFactory {
  public:
    explicit LegacyFactory(const std::string& config_file);

    ndk::ScopedAStatus queryEffects(
            const std::optional<::aidl::android::media::audio::common::AudioUuid>& type,
            const std::optional<::aidl::android::media::audio::common::AudioUuid>& implementation,
            const std::optional<::aidl::android::media::audio::common::AudioUuid>& proxy,
            std::vector<Descriptor>* descriptors) override;
    ndk::ScopedAStatus queryProcessing(const std::optional<Processing::Type>& type,
                                       std::vector<Processing>* processings) override;
    ndk::ScopedAStatus createEffect(
            const ::aidl::android::media::audio::common::AudioUuid& implementation,
            std::shared_ptr<IEffect>* effect) override;
    ndk::ScopedAStatus destroyEffect(const std::shared_ptr<IEffect>& effect) override;
    binder_status_t dump(int fd, const char** args, uint32_t num_args) override;

    size_t effect_count() const { return effects_.size(); }

  private:
    // One usable effect implementation.
    struct Entry {
        std::shared_ptr<LegacyLibrary> library;
        effect_uuid_t legacy_uuid;
        Descriptor descriptor;  // AIDL descriptor including proxy and capability.
    };

    void LoadLibraries();
    void LoadEffects();
    // Builds the AIDL descriptor of one implementation, creating a throw-away
    // instance to fill the capability. Returns nullopt when the library does
    // not know the UUID.
    std::optional<Entry> MakeEntry(
            const EffectConfig::Library& library,
            const std::optional<::aidl::android::media::audio::common::AudioUuid>& proxy);
    const Entry* FindEntry(const ::aidl::android::media::audio::common::AudioUuid& uuid) const
            REQUIRES(lock_);

    const EffectConfig config_;
    mutable std::mutex lock_;
    std::map<std::string, std::shared_ptr<LegacyLibrary>> libraries_ GUARDED_BY(lock_);
    // Keyed by the implementation UUID (as string, AudioUuid has no operator<).
    std::map<std::string, Entry> effects_ GUARDED_BY(lock_);
    // Live instances, so that destroyEffect() can validate its argument.
    std::vector<std::weak_ptr<IEffect>> instances_ GUARDED_BY(lock_);
};

}  // namespace aidl::android::hardware::audio::effect::legacy
