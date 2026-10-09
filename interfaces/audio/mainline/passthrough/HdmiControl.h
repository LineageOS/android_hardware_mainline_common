/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

// The effective high bit rate support of a head and what decided it.
struct HbrState {
    enum class Source {
        kDriver,   // The quirk of the card's driver.
        kSetting,  // The card's "hbr" setting forces it on or off.
        kFailure,  // An attempt on this head was refused, see `detail`.
    };
    HbrSupport support = HbrSupport::kUndetectable;
    Source source = Source::kDriver;
    // The quirk's name, or what was refused and how.
    std::string detail;

    std::string ToString() const;
};

const char* ToString(HbrState::Source source);

// Restores the channel status that was set before, when destroyed.
class ChannelStatusGuard {
  public:
    virtual ~ChannelStatusGuard() = default;
};

// What the HAL learned about its HDMI heads at run time: which refused high
// bit rate, and why. Survives the streams and the HdmiControl instances,
// which are short lived; forgotten when HDMI connects again (a new sink, or
// the same one after a replug). Thread safe.
class HeadStates {
  public:
    void SetHbrFailure(const std::string& head, std::string reason);
    // The reason, nullopt when the head did not refuse high bit rate.
    std::optional<std::string> HbrFailure(const std::string& head) const;
    // Returns whether there was something to forget.
    bool ClearHbrFailures();

  private:
    mutable std::mutex lock_;
    std::map<std::string, std::string> hbr_failures_ GUARDED_BY(lock_);
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
    virtual HbrState Hbr() const = 0;
    // The head refused high bit rate (`reason`: what and how): remember it.
    virtual void ReportHbrFailure(const std::string& reason) = 0;
    // For logs and dumpsys.
    virtual std::string Describe() const = 0;
};

// The HdmiControl of an HDMI head (an endpoint with is_hdmi_head) on `card`.
// Returns nullptr when passthrough is not possible on the head (the reason is
// logged). `hbr_override` is the card's "hbr" property, if set.
std::unique_ptr<HdmiControl> CreateHdmiControl(const alsa::CardInfo& card,
                                               const routing::Endpoint& head,
                                               std::shared_ptr<HeadStates> head_states,
                                               std::optional<bool> hbr_override);

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
