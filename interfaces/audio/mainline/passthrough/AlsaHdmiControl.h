/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <alsa/asoundlib.h>

#include "alsa/AlsaCard.h"
#include "passthrough/HdmiControl.h"
#include "passthrough/Quirks.h"
#include "routing/Endpoint.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

// HdmiControl on top of the "ELD" and "IEC958 Playback Default" controls of
// an ALSA card, found through the locators of the card's quirk.
class AlsaHdmiControl final : public HdmiControl {
  public:
    // A control found by a locator.
    struct Control {
        unsigned int numid = 0;
        ControlLocator locator = ControlLocator::kPcmDevice;
        std::string description;
    };

    // Returns nullptr when the head has no usable IEC958 status control.
    static std::unique_ptr<AlsaHdmiControl> Create(const alsa::CardInfo& card,
                                                   const routing::Endpoint& head,
                                                   const Quirk& quirk,
                                                   std::shared_ptr<HbrFailures> hbr_failures,
                                                   std::optional<bool> hbr_override);

    AlsaHdmiControl(std::shared_ptr<snd_ctl_t> ctl, std::string head_key, Quirk quirk,
                    std::optional<Control> eld, Control iec958,
                    std::shared_ptr<HbrFailures> hbr_failures, std::optional<bool> hbr_override);

    std::optional<std::vector<uint8_t>> ReadEld() override;
    std::unique_ptr<ChannelStatusGuard> SetChannelStatus(const Iec958Status& status) override;
    void ClearNonAudio() override;
    HbrSupport Hbr() const override;
    void ReportHbrFailure() override;
    std::string Describe() const override;

  private:
    const std::shared_ptr<snd_ctl_t> ctl_;
    // Identifies the head across instances, for the HBR failure registry.
    const std::string head_key_;
    const Quirk quirk_;
    const std::optional<Control> eld_;
    const Control iec958_;
    const std::shared_ptr<HbrFailures> hbr_failures_;
    const std::optional<bool> hbr_override_;
};

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
