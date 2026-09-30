/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ANDROID_AUDIO_DTSHD_FRAME_SCANNER_H
#define ANDROID_AUDIO_DTSHD_FRAME_SCANNER_H

#include <stdint.h>
#include <audio_utils/spdif/FrameScanner.h>
#include <audio_utils/spdif/SPDIF.h>

#include "DTSFrameScanner.h"

namespace android {

// Longest repetition period of an IEC 61937-5 type IV burst, in IEC frames
// (DTS-HD sub-type 5).
#define DTSHD_MAX_REPETITION_PERIOD        16384

/**
 * Scanner for DTS-HD streams (DTS-HD High Resolution Audio and Master Audio),
 * sent as IEC 61937-5 type IV bursts at a multiple of the core sample rate.
 *
 * A DTS-HD frame is a DTS core frame followed by an extension substream frame
 * that covers the same audio. Both go into one data burst: the core frame is
 * the first frame of the burst, the extension substream the last one. An
 * extension substream without a preceding core is skipped, so streams without
 * a core (e.g. DTS Express) are not supported.
 */
class DTSHDFrameScanner : public DTSFrameScanner
{
public:
    /**
     * @param rateMultiplier IEC 61937 rate multiplier: 4 fits DTS-HD High
     *   Resolution Audio into a stereo stream, 16 (high bit rate) is needed for
     *   Master Audio.
     */
    explicit DTSHDFrameScanner(uint32_t rateMultiplier);
    virtual ~DTSHDFrameScanner();

    virtual bool scan(uint8_t byte);

    virtual int getMaxChannels() const { return 7 + 1; }

    virtual int getMaxSampleFramesPerSyncFrame() const {
        return DTSHD_MAX_REPETITION_PERIOD;
    }

    // The repetition period of the burst, in IEC frames.
    virtual int getSampleFramesPerSyncFrame() const {
        return mSampleFramesPerSyncFrame * mRateMultiplier;
    }

    virtual bool isFirstInBurst() { return !mIsExtensionSubstream; }
    virtual bool isLastInBurst() { return mIsExtensionSubstream; }

    // Type IV bursts count their length in bytes.
    virtual uint16_t convertBytesToLengthCode(uint16_t numBytes) const { return numBytes; }

protected:
    const uint32_t mHdRateMultiplier;
    // Header bytes DTSFrameScanner parses for a core frame.
    const uint32_t mCoreHeaderLength;
    // The frame found by the last successful scan() is an extension substream.
    bool mIsExtensionSubstream;
    // The last frame found was a core frame, so an extension substream may
    // follow it.
    bool mCoreSeen;

    virtual bool parseHeader();
    bool parseExtensionSubstreamHeader();

    static const uint8_t kExtensionSubstreamSyncBytes[];
};

}  // namespace android
#endif  // ANDROID_AUDIO_DTSHD_FRAME_SCANNER_H
