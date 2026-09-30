/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <android-base/thread_annotations.h>

#include "alsa/AlsaPcm.h"

namespace aidl::android::hardware::audio::core::mainline::routing {

// Decides which output stream may open an exclusive PCM device (one with a
// single substream) when several want it at once.
//
// The audio policy keeps "primary output" open and routed while a direct
// output (hra, multichannel, passthrough) plays to the same device, and
// AudioFlinger keeps writing silence to it for a few seconds before standby.
// Both HAL streams then open the same kernel device and the second one gets
// -EBUSY. The arbiter lets the direct stream win: the mixed stream is asked to
// yield, closes its PCM and continues on a paced null path until the direct
// stream releases the device again.
//
// Only playback is arbitrated, and only PCMs whose identity is known and
// exclusive. All methods are thread safe; the arbiter never calls back into a
// stream, it only raises the client's yield flag, which the stream's worker
// thread polls.
class PcmArbiter {
  public:
    enum class Priority {
        kMixed,   // Mix ports without the DIRECT flag: yield to direct streams.
        kDirect,  // DIRECT mix ports: pre-empt mixed streams.
    };

    // One stream's handle.
    class Client {
      public:
        Client(std::string name, Priority priority, int32_t burst_ms)
            : name_(std::move(name)), priority_(priority), burst_ms_(burst_ms) {}

        const std::string& name() const { return name_; }
        Priority priority() const { return priority_; }

        // Worker thread: true (once) when another client asked this one to
        // give up a device since the last call. PendingYields() tells which.
        bool TakeYieldRequest() { return yield_requested_.exchange(false); }

      private:
        friend class PcmArbiter;

        const std::string name_;
        const Priority priority_;
        // Time between two transfers of the stream, i.e. how long it may take
        // at most to notice a yield request.
        const int32_t burst_ms_;
        std::atomic<bool> yield_requested_ = false;
    };

    enum class Result {
        kGranted,       // The client owns the device now (or already did).
        kBusy,          // Owned or claimed by a client with the same or a higher priority.
        kWaitForYield,  // A lower priority owner was asked to yield; call Acquire() again.
    };

    // The longest a pre-empting client waits for the owner to yield, on top of
    // two of the owner's bursts.
    static constexpr int32_t kYieldMarginMs = 100;

    // Asks for `pcm`. On kWaitForYield, `yield_timeout_ms` is how long the
    // caller should keep retrying before giving up (and calling Release()).
    Result Acquire(const std::shared_ptr<Client>& client, const alsa::PcmIdentity& pcm,
                   int32_t* yield_timeout_ms);
    // Gives up ownership of, or a pending claim on, `pcm`. Call only after the
    // device has been closed.
    void Release(const Client& client, const alsa::PcmIdentity& pcm);
    // Release() for every device the client owns or claims.
    void ReleaseAll(const Client& client);
    // Devices the client owns and another client is waiting for.
    std::vector<alsa::PcmIdentity> PendingYields(const Client& client) const;

    std::string Dump() const;

  private:
    struct Entry {
        std::shared_ptr<Client> owner;
        // Higher priority client waiting for `owner` to yield. It keeps the
        // device reserved until it acquires it or gives up.
        std::shared_ptr<Client> claimant;
    };

    void ReleaseLocked(const Client& client, std::map<alsa::PcmIdentity, Entry>::iterator it)
            REQUIRES(lock_);

    mutable std::mutex lock_;
    std::map<alsa::PcmIdentity, Entry> entries_ GUARDED_BY(lock_);
};

}  // namespace aidl::android::hardware::audio::core::mainline::routing
