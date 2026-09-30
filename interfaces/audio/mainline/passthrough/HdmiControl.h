/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <android-base/thread_annotations.h>

#include "alsa/AlsaCard.h"
#include "passthrough/Format.h"
#include "routing/Endpoint.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// Whether a head can send high bit rate (8 channels at 192 kHz of IEC 61937
// data), as far as the HAL can tell.
enum class HbrSupport {
    kSupported,     // Use it; a failure at open time turns it off.
    kUnsupported,   // Known not to work: off.
    kUndetectable,  // An attempt may "succeed" and send garbage: off unless forced.
};

const char* ToString(HbrSupport hbr);

// Restores the channel status that was set before, when destroyed.
class ChannelStatusGuard {
  public:
    virtual ~ChannelStatusGuard() = default;
};

// Heads on which a high bit rate stream failed to open. Survives the streams
// and the HdmiControl instances, which are short lived. Thread safe.
class HbrFailures {
  public:
    void Add(const std::string& head);
    bool Contains(const std::string& head) const;

  private:
    mutable std::mutex lock_;
    std::set<std::string> heads_ GUARDED_BY(lock_);
};

// Vendor boundary of HDMI passthrough: everything that depends on where a
// driver puts the sink's ELD and the IEC 958 channel status, and on whether
// it can do high bit rate. The rest of the passthrough code is vendor
// neutral. Instances belong to one thread at a time.
class HdmiControl {
  public:
    virtual ~HdmiControl() = default;

    // The sink's ELD, nullopt when there is none (no sink) or it can not be
    // read.
    virtual std::optional<std::vector<uint8_t>> ReadEld() = 0;
    // Sets the IEC 958 channel status of the head. Must happen before the
    // PCM device is opened and prepared: drivers (HDA) sample it there.
    // Returns nullptr on failure.
    virtual std::unique_ptr<ChannelStatusGuard> SetChannelStatus(const Iec958Status& status) = 0;
    // Clears a non-audio flag another user (e.g. a crashed HAL) left set, so
    // that PCM playback is not taken for compressed data.
    virtual void ClearNonAudio() = 0;
    // Effective high bit rate support: quirk, property override and runtime
    // failures combined. Only kSupported means usable.
    virtual HbrSupport Hbr() const = 0;
    // A high bit rate stream failed to open on this head: remember it.
    virtual void ReportHbrFailure() = 0;
    // For logs and dumpsys.
    virtual std::string Describe() const = 0;
};

// The HdmiControl of an HDMI head (an endpoint with is_hdmi_head) on `card`.
// Returns nullptr when passthrough is not possible on the head (the reason is
// logged). `hbr_override` is the card's "hbr" property, if set.
std::unique_ptr<HdmiControl> CreateHdmiControl(const alsa::CardInfo& card,
                                               const routing::Endpoint& head,
                                               std::shared_ptr<HbrFailures> hbr_failures,
                                               std::optional<bool> hbr_override);

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
