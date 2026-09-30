/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_PcmArbiter"

#include "routing/PcmArbiter.h"

#include <iterator>
#include <sstream>

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core::mainline::routing {

PcmArbiter::Result PcmArbiter::Acquire(const std::shared_ptr<Client>& client,
                                       const alsa::PcmIdentity& pcm, int32_t* yield_timeout_ms) {
    std::lock_guard guard(lock_);
    Entry& entry = entries_[pcm];
    if (entry.owner == client) return Result::kGranted;

    if (entry.owner == nullptr) {
        // A device given up for a claimant stays reserved for it.
        if (entry.claimant != nullptr && entry.claimant != client) return Result::kBusy;
        entry.owner = client;
        entry.claimant = nullptr;
        LOG(INFO) << __func__ << ": " << pcm.ToString() << " owned by " << client->name();
        return Result::kGranted;
    }

    if (client->priority() > entry.owner->priority() &&
        (entry.claimant == nullptr || entry.claimant == client)) {
        if (entry.claimant == nullptr) {
            LOG(INFO) << __func__ << ": " << client->name() << " asks " << entry.owner->name()
                      << " to yield " << pcm.ToString();
            entry.claimant = client;
        }
        entry.owner->yield_requested_.store(true);
        *yield_timeout_ms = 2 * entry.owner->burst_ms_ + kYieldMarginMs;
        return Result::kWaitForYield;
    }
    return Result::kBusy;
}

void PcmArbiter::ReleaseLocked(const Client& client,
                               std::map<alsa::PcmIdentity, Entry>::iterator it) {
    Entry& entry = it->second;
    if (entry.owner.get() == &client) {
        entry.owner = nullptr;
        LOG(INFO) << "Release: " << it->first.ToString() << " released by " << client.name()
                  << (entry.claimant != nullptr ? " for " + entry.claimant->name() : "");
    }
    if (entry.claimant.get() == &client) {
        entry.claimant = nullptr;
        LOG(INFO) << "Release: " << client.name() << " withdrew its claim on "
                  << it->first.ToString();
    }
    if (entry.owner == nullptr && entry.claimant == nullptr) entries_.erase(it);
}

void PcmArbiter::Release(const Client& client, const alsa::PcmIdentity& pcm) {
    std::lock_guard guard(lock_);
    if (const auto it = entries_.find(pcm); it != entries_.end()) ReleaseLocked(client, it);
}

void PcmArbiter::ReleaseAll(const Client& client) {
    std::lock_guard guard(lock_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        const auto next = std::next(it);
        ReleaseLocked(client, it);
        it = next;
    }
}

std::vector<alsa::PcmIdentity> PcmArbiter::PendingYields(const Client& client) const {
    std::lock_guard guard(lock_);
    std::vector<alsa::PcmIdentity> result;
    for (const auto& [pcm, entry] : entries_) {
        if (entry.owner.get() == &client && entry.claimant != nullptr) result.push_back(pcm);
    }
    return result;
}

std::string PcmArbiter::Dump() const {
    std::lock_guard guard(lock_);
    std::ostringstream os;
    os << "PcmArbiter: " << entries_.size() << " device(s) in use\n";
    for (const auto& [pcm, entry] : entries_) {
        os << "  " << pcm.ToString()
           << ": owner=" << (entry.owner != nullptr ? entry.owner->name() : "none");
        if (entry.claimant != nullptr) os << " claimed by " << entry.claimant->name();
        os << "\n";
    }
    return os.str();
}

}  // namespace aidl::android::hardware::audio::core::mainline::routing
