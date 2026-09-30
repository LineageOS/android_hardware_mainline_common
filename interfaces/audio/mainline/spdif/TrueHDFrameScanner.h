/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ANDROID_AUDIO_TRUEHD_FRAME_SCANNER_H
#define ANDROID_AUDIO_TRUEHD_FRAME_SCANNER_H

#include <stdint.h>
#include <audio_utils/spdif/FrameScanner.h>
#include <audio_utils/spdif/SPDIF.h>

namespace android {

// Length of a MAT data burst (preamble, MAT frame and stuffing) in bytes, i.e.
// its repetition period in two channel 16-bit IEC frames times 4.
#define MAT_BURST_BYTES                  61440
// Bytes of MAT stream per TrueHD access unit of 40 samples.
#define MAT_BYTES_PER_40_SAMPLES          2560

/**
 * Finds the access units of a Dolby TrueHD stream, which the SPDIFEncoder
 * packs into MAT frames (IEC 61937-9, data type 22) at 16 times the base
 * sample rate (high bit rate).
 *
 * Access units have no sync word of their own, only the major sync units
 * that start a group of them do. The scanner therefore waits for a major sync
 * and then follows the chain of access unit lengths. resetBurst() drops the
 * lock, so that a reset of the encoder (e.g. after a seek) waits for the next
 * major sync.
 */
class TrueHDFrameScanner : public FrameScanner
{
public:
    TrueHDFrameScanner();
    virtual ~TrueHDFrameScanner();

    virtual bool scan(uint8_t byte);

    virtual int getMaxChannels() const { return 7 + 1; }

    // MAT bursts have a fixed length, whatever the access units in them.
    virtual int getMaxSampleFramesPerSyncFrame() const {
        return MAT_BURST_BYTES / (sizeof(uint16_t) * kSpdifEncodedChannelCount);
    }
    virtual int getSampleFramesPerSyncFrame() const {
        return getMaxSampleFramesPerSyncFrame();
    }

    // The SPDIFEncoder decides about MAT bursts on its own.
    virtual bool isFirstInBurst() { return false; }
    virtual bool isLastInBurst() { return false; }
    virtual void resetBurst();

    // MAT bursts count their length in bytes.
    virtual uint16_t convertBytesToLengthCode(uint16_t numBytes) const { return numBytes; }

    /**
     * @return input timing of the access unit found by the last scan(), in
     *   samples at the stream's sample rate (modulo 2^16)
     */
    uint16_t getInputTiming() const { return mInputTiming; }

    /**
     * @return number of samples in an access unit (40 at 44.1 / 48 kHz, 80 or
     *   160 at the multiples)
     */
    uint32_t getSamplesPerAccessUnit() const { return mSamplesPerAccessUnit; }

protected:
    enum {
        ACCESS_UNIT_HEADER_BYTES = 4,
        MAJOR_SYNC_HEADER_BYTES = 12,
    };

    // Following the chain of access units after a major sync.
    bool mLocked;
    uint16_t mInputTiming;
    uint32_t mSamplesPerAccessUnit;

    bool isMajorSync() const;
    virtual bool parseHeader();

    static const uint8_t kMajorSyncBytes[];
};

}  // namespace android
#endif  // ANDROID_AUDIO_TRUEHD_FRAME_SCANNER_H
