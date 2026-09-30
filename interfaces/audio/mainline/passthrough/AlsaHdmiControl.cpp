/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_Passthrough"

#include "passthrough/AlsaHdmiControl.h"

#include <cstring>
#include <sstream>

#include <android-base/logging.h>

#include "alsa/AlsaError.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

namespace {

constexpr const char* kEldControl = "ELD";
constexpr const char* kIec958Control = "IEC958 Playback Default";

struct ElemValueFreer {
    void operator()(snd_ctl_elem_value_t* value) const { snd_ctl_elem_value_free(value); }
};
using ElemValuePtr = std::unique_ptr<snd_ctl_elem_value_t, ElemValueFreer>;

struct ElemInfoFreer {
    void operator()(snd_ctl_elem_info_t* info) const { snd_ctl_elem_info_free(info); }
};
using ElemInfoPtr = std::unique_ptr<snd_ctl_elem_info_t, ElemInfoFreer>;

struct ElemListFreer {
    void operator()(snd_ctl_elem_list_t* list) const {
        snd_ctl_elem_list_free_space(list);
        snd_ctl_elem_list_free(list);
    }
};
using ElemListPtr = std::unique_ptr<snd_ctl_elem_list_t, ElemListFreer>;

ElemValuePtr AllocValue(unsigned int numid) {
    snd_ctl_elem_value_t* value = nullptr;
    if (snd_ctl_elem_value_malloc(&value) < 0) return nullptr;
    snd_ctl_elem_value_set_numid(value, numid);
    return ElemValuePtr(value);
}

// One element of the card's control list.
struct Element {
    unsigned int numid;
    snd_ctl_elem_iface_t iface;
    unsigned int device;
    unsigned int index;
    std::string name;
};

std::vector<Element> ListElements(snd_ctl_t* ctl) {
    std::vector<Element> elements;
    snd_ctl_elem_list_t* raw = nullptr;
    if (snd_ctl_elem_list_malloc(&raw) < 0) return elements;
    ElemListPtr list(raw);
    if (int err = snd_ctl_elem_list(ctl, raw); err < 0) {
        LOG(WARNING) << __func__ << ": snd_ctl_elem_list: " << alsa::ErrorString(err);
        return elements;
    }
    const unsigned int count = snd_ctl_elem_list_get_count(raw);
    if (int err = snd_ctl_elem_list_alloc_space(raw, count); err < 0) return elements;
    if (int err = snd_ctl_elem_list(ctl, raw); err < 0) {
        LOG(WARNING) << __func__ << ": snd_ctl_elem_list: " << alsa::ErrorString(err);
        return elements;
    }
    for (unsigned int i = 0; i < snd_ctl_elem_list_get_used(raw); ++i) {
        elements.push_back(Element{.numid = snd_ctl_elem_list_get_numid(raw, i),
                                   .iface = snd_ctl_elem_list_get_interface(raw, i),
                                   .device = snd_ctl_elem_list_get_device(raw, i),
                                   .index = snd_ctl_elem_list_get_index(raw, i),
                                   .name = snd_ctl_elem_list_get_name(raw, i)});
    }
    return elements;
}

std::string Describe(const Element& element) {
    std::ostringstream os;
    os << "numid=" << element.numid << ",iface=" << snd_ctl_elem_iface_name(element.iface)
       << ",device=" << element.device << ",index=" << element.index << ",name='" << element.name
       << "'";
    return os.str();
}

// Position of the head's PCM among the card's HDMI playback PCMs, which the
// card lists by device number. -1 when the head is not among them.
int HdmiOrdinal(const alsa::CardInfo& card, int device) {
    int ordinal = 0;
    for (const alsa::PcmDeviceInfo& pcm : card.pcms) {
        if (!pcm.playback || !pcm.LooksLikeHdmi()) continue;
        if (pcm.device == device) return ordinal;
        ++ordinal;
    }
    return -1;
}

std::optional<AlsaHdmiControl::Control> Locate(const std::vector<Element>& elements,
                                               const char* name,
                                               const std::vector<ControlLocator>& locators,
                                               int device, int ordinal) {
    std::vector<const Element*> candidates;
    for (const Element& element : elements) {
        if (element.name == name) candidates.push_back(&element);
    }
    for (const ControlLocator locator : locators) {
        const Element* found = nullptr;
        switch (locator) {
            case ControlLocator::kPcmDevice:
                for (const Element* e : candidates) {
                    if (device >= 0 && e->iface == SND_CTL_ELEM_IFACE_PCM &&
                        e->device == static_cast<unsigned int>(device)) {
                        found = e;
                        break;
                    }
                }
                break;
            case ControlLocator::kHdaOrdinal:
                for (const Element* e : candidates) {
                    if (ordinal >= 0 && e->iface == SND_CTL_ELEM_IFACE_MIXER &&
                        e->index == static_cast<unsigned int>(ordinal)) {
                        found = e;
                        break;
                    }
                }
                break;
            case ControlLocator::kSoleControl:
                if (candidates.size() == 1) found = candidates.front();
                break;
        }
        if (found != nullptr) {
            return AlsaHdmiControl::Control{
                    .numid = found->numid, .locator = locator, .description = Describe(*found)};
        }
    }
    return std::nullopt;
}

std::optional<Iec958Status> ReadStatus(snd_ctl_t* ctl, unsigned int numid) {
    ElemValuePtr value = AllocValue(numid);
    if (value == nullptr) return std::nullopt;
    if (int err = snd_ctl_elem_read(ctl, value.get()); err < 0) {
        LOG(WARNING) << __func__ << ": numid " << numid << ": " << alsa::ErrorString(err);
        return std::nullopt;
    }
    snd_aes_iec958_t aes = {};
    snd_ctl_elem_value_get_iec958(value.get(), &aes);
    Iec958Status status;
    static_assert(sizeof(aes.status) == sizeof(status.bytes));
    std::memcpy(status.bytes.data(), aes.status, status.bytes.size());
    return status;
}

bool WriteStatus(snd_ctl_t* ctl, unsigned int numid, const Iec958Status& status) {
    ElemValuePtr value = AllocValue(numid);
    if (value == nullptr) return false;
    // Keep the rest of the value (subcode, ...) as it is.
    if (int err = snd_ctl_elem_read(ctl, value.get()); err < 0) {
        LOG(WARNING) << __func__ << ": numid " << numid << ": " << alsa::ErrorString(err);
        return false;
    }
    snd_aes_iec958_t aes = {};
    snd_ctl_elem_value_get_iec958(value.get(), &aes);
    std::memcpy(aes.status, status.bytes.data(), status.bytes.size());
    snd_ctl_elem_value_set_iec958(value.get(), &aes);
    if (int err = snd_ctl_elem_write(ctl, value.get()); err < 0) {
        LOG(ERROR) << __func__ << ": numid " << numid << " = " << status.ToString() << ": "
                   << alsa::ErrorString(err);
        return false;
    }
    return true;
}

class AlsaChannelStatusGuard final : public ChannelStatusGuard {
  public:
    AlsaChannelStatusGuard(std::shared_ptr<snd_ctl_t> ctl, unsigned int numid,
                           Iec958Status previous)
        : ctl_(std::move(ctl)), numid_(numid), previous_(previous) {}
    ~AlsaChannelStatusGuard() override {
        if (WriteStatus(ctl_.get(), numid_, previous_)) {
            LOG(DEBUG) << "restored channel status " << previous_.ToString();
        }
    }

  private:
    const std::shared_ptr<snd_ctl_t> ctl_;
    const unsigned int numid_;
    const Iec958Status previous_;
};

}  // namespace

std::unique_ptr<AlsaHdmiControl> AlsaHdmiControl::Create(const alsa::CardInfo& card,
                                                         const routing::Endpoint& head,
                                                         const Quirk& quirk,
                                                         std::shared_ptr<HbrFailures> hbr_failures,
                                                         std::optional<bool> hbr_override) {
    snd_ctl_t* raw = nullptr;
    if (int err = snd_ctl_open(&raw, card.CtlName().c_str(), 0); err < 0) {
        LOG(WARNING) << __func__ << ": snd_ctl_open(" << card.CtlName()
                     << "): " << alsa::ErrorString(err);
        return nullptr;
    }
    std::shared_ptr<snd_ctl_t> ctl(raw, [](snd_ctl_t* c) { snd_ctl_close(c); });

    const int device = head.pcm_identity.IsKnown() ? head.pcm_identity.device : -1;
    const int ordinal = device >= 0 ? HdmiOrdinal(card, device) : -1;
    const std::vector<Element> elements = ListElements(raw);
    auto eld = Locate(elements, kEldControl, quirk.locators, device, ordinal);
    auto iec958 = Locate(elements, kIec958Control, quirk.locators, device, ordinal);
    LOG(INFO) << __func__ << ": " << head.name << " (" << head.pcm_name << ", pcm device " << device
              << ", HDMI ordinal " << ordinal << ") on card " << card.index << " [" << card.driver
              << "], quirk " << quirk.name << ": ELD "
              << (eld.has_value() ? eld->description : "not found") << ", IEC958 "
              << (iec958.has_value() ? iec958->description : "not found");
    if (!iec958.has_value()) {
        LOG(WARNING) << __func__ << ": no \"" << kIec958Control << "\" control for " << head.name
                     << ", no passthrough on it";
        return nullptr;
    }
    return std::make_unique<AlsaHdmiControl>(std::move(ctl), card.id + ":" + head.pcm_name, quirk,
                                             std::move(eld), std::move(*iec958),
                                             std::move(hbr_failures), hbr_override);
}

AlsaHdmiControl::AlsaHdmiControl(std::shared_ptr<snd_ctl_t> ctl, std::string head_key, Quirk quirk,
                                 std::optional<Control> eld, Control iec958,
                                 std::shared_ptr<HbrFailures> hbr_failures,
                                 std::optional<bool> hbr_override)
    : ctl_(std::move(ctl)),
      head_key_(std::move(head_key)),
      quirk_(std::move(quirk)),
      eld_(std::move(eld)),
      iec958_(std::move(iec958)),
      hbr_failures_(std::move(hbr_failures)),
      hbr_override_(hbr_override) {}

std::optional<std::vector<uint8_t>> AlsaHdmiControl::ReadEld() {
    if (!eld_.has_value()) return std::nullopt;
    snd_ctl_elem_info_t* raw_info = nullptr;
    if (snd_ctl_elem_info_malloc(&raw_info) < 0) return std::nullopt;
    ElemInfoPtr info(raw_info);
    snd_ctl_elem_info_set_numid(raw_info, eld_->numid);
    if (int err = snd_ctl_elem_info(ctl_.get(), raw_info); err < 0) {
        LOG(WARNING) << __func__ << ": " << head_key_ << ": " << alsa::ErrorString(err);
        return std::nullopt;
    }
    const unsigned int count = snd_ctl_elem_info_get_count(raw_info);
    if (snd_ctl_elem_info_get_type(raw_info) != SND_CTL_ELEM_TYPE_BYTES || count == 0) {
        return std::nullopt;
    }
    ElemValuePtr value = AllocValue(eld_->numid);
    if (value == nullptr) return std::nullopt;
    if (int err = snd_ctl_elem_read(ctl_.get(), value.get()); err < 0) {
        LOG(WARNING) << __func__ << ": " << head_key_ << ": " << alsa::ErrorString(err);
        return std::nullopt;
    }
    const auto* bytes = static_cast<const uint8_t*>(snd_ctl_elem_value_get_bytes(value.get()));
    return std::vector<uint8_t>(bytes, bytes + count);
}

std::unique_ptr<ChannelStatusGuard> AlsaHdmiControl::SetChannelStatus(const Iec958Status& status) {
    const auto previous = ReadStatus(ctl_.get(), iec958_.numid);
    if (!previous.has_value() || !WriteStatus(ctl_.get(), iec958_.numid, status)) {
        return nullptr;
    }
    LOG(DEBUG) << __func__ << ": " << head_key_ << ": " << previous->ToString() << " -> "
               << status.ToString();
    return std::make_unique<AlsaChannelStatusGuard>(ctl_, iec958_.numid, *previous);
}

void AlsaHdmiControl::ClearNonAudio() {
    auto status = ReadStatus(ctl_.get(), iec958_.numid);
    if (!status.has_value() || !status->IsNonAudio()) return;
    LOG(WARNING) << __func__ << ": " << head_key_ << ": channel status " << status->ToString()
                 << " was left non-audio, clearing it";
    status->bytes[0] &= ~IEC958_AES0_NONAUDIO;
    WriteStatus(ctl_.get(), iec958_.numid, *status);
}

HbrSupport AlsaHdmiControl::Hbr() const {
    // A failure beats everything: forcing it on again would fail again.
    if (hbr_failures_ != nullptr && hbr_failures_->Contains(head_key_)) {
        return HbrSupport::kUnsupported;
    }
    if (hbr_override_.has_value()) {
        return *hbr_override_ ? HbrSupport::kSupported : HbrSupport::kUnsupported;
    }
    return quirk_.hbr;
}

void AlsaHdmiControl::ReportHbrFailure() {
    LOG(WARNING) << __func__ << ": " << head_key_
                 << ": high bit rate failed, not offering it on this head any more";
    if (hbr_failures_ != nullptr) hbr_failures_->Add(head_key_);
}

std::string AlsaHdmiControl::Describe() const {
    std::ostringstream os;
    os << head_key_ << " quirk=" << quirk_.name << " ELD="
       << (eld_.has_value() ? std::string(ToString(eld_->locator)) + "{" + eld_->description + "}"
                            : "none")
       << " IEC958=" << ToString(iec958_.locator) << "{" << iec958_.description << "}"
       << " hbr=" << ToString(Hbr());
    return os.str();
}

// --- Shared parts of the vendor boundary -------------------------------------

const char* ToString(HbrSupport hbr) {
    switch (hbr) {
        case HbrSupport::kSupported:
            return "supported";
        case HbrSupport::kUnsupported:
            return "unsupported";
        case HbrSupport::kUndetectable:
            return "undetectable";
    }
    return "?";
}

void HbrFailures::Add(const std::string& head) {
    std::lock_guard guard(lock_);
    heads_.insert(head);
}

bool HbrFailures::Contains(const std::string& head) const {
    std::lock_guard guard(lock_);
    return heads_.count(head) != 0;
}

std::unique_ptr<HdmiControl> CreateHdmiControl(const alsa::CardInfo& card,
                                               const routing::Endpoint& head,
                                               std::shared_ptr<HbrFailures> hbr_failures,
                                               std::optional<bool> hbr_override) {
    if (head.IsNull() || !head.is_hdmi_head || head.card != card.index) return nullptr;
    // A vendor whose driver the quirk table can not describe gets its own
    // HdmiControl implementation, chosen here.
    return AlsaHdmiControl::Create(card, head, QuirkFor(card), std::move(hbr_failures),
                                   hbr_override);
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
