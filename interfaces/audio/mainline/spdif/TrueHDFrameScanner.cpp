/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AudioSPDIF"
//#define LOG_NDEBUG 0

#include <string.h>

#include <log/log.h>
#include <audio_utils/spdif/FrameScanner.h>

#include "TrueHDFrameScanner.h"

namespace android {

// Major sync of a TrueHD access unit, at byte 4 of the unit. The last byte is
// 0xBA for TrueHD and 0xBB for MLP.
const uint8_t TrueHDFrameScanner::kMajorSyncBytes[] = { 0xF8, 0x72, 0x6F, 0xBA };

TrueHDFrameScanner::TrueHDFrameScanner()
 : FrameScanner(kSpdifDataTypeMat,
    TrueHDFrameScanner::kMajorSyncBytes,
    sizeof(TrueHDFrameScanner::kMajorSyncBytes),
    MAJOR_SYNC_HEADER_BYTES)
 , mLocked(false)
 , mInputTiming(0)
 , mSamplesPerAccessUnit(0)
{
    mSampleRate = 48000;
    mRateMultiplier = kSpdifRateMultiplierHbr;
}

TrueHDFrameScanner::~TrueHDFrameScanner()
{
}

void TrueHDFrameScanner::resetBurst()
{
    mLocked = false;
    mCursor = 0;
    mHeaderLength = MAJOR_SYNC_HEADER_BYTES;
}

bool TrueHDFrameScanner::isMajorSync() const
{
    return memcmp(&mHeaderBuffer[ACCESS_UNIT_HEADER_BYTES], kMajorSyncBytes,
                    sizeof(kMajorSyncBytes) - 1) == 0
            && (mHeaderBuffer[7] == 0xBA || mHeaderBuffer[7] == 0xBB);
}

// Collects the header of the next access unit: before the lock a window of
// the stream that holds a major sync, afterwards the bytes following the
// previous access unit.
bool TrueHDFrameScanner::scan(uint8_t byte)
{
    if (!mLocked) {
        if (mCursor == MAJOR_SYNC_HEADER_BYTES) {
            memmove(mHeaderBuffer, &mHeaderBuffer[1], MAJOR_SYNC_HEADER_BYTES - 1);
            mCursor--;
            mBytesSkipped++;
        }
        mHeaderBuffer[mCursor++] = byte;
        if (mCursor < MAJOR_SYNC_HEADER_BYTES || !isMajorSync()) {
            return false;
        }
        mHeaderLength = MAJOR_SYNC_HEADER_BYTES;
    } else {
        if (mCursor == 0) {
            mHeaderLength = ACCESS_UNIT_HEADER_BYTES;
        }
        mHeaderBuffer[mCursor++] = byte;
        if (mCursor == ACCESS_UNIT_HEADER_BYTES) {
            // The length is known now. Read on if the unit is long enough to
            // start with a major sync, which tells the sample rate.
            const uint32_t length = ((mHeaderBuffer[0] & 0x0F) << 8 | mHeaderBuffer[1]) * 2;
            if (length >= MAJOR_SYNC_HEADER_BYTES) {
                mHeaderLength = MAJOR_SYNC_HEADER_BYTES;
            }
        }
        if (mCursor < mHeaderLength) {
            return false;
        }
    }
    mCursor = 0;
    if (!parseHeader()) {
        ALOGE("TrueHDFrameScanner: ERROR - parseHeader() failed, waiting for a major sync");
        resetBurst();
        return false;
    }
    mLocked = true;
    return true;
}

bool TrueHDFrameScanner::parseHeader()
{
    // Access unit header: check nibble (4 bits), access unit length in 16-bit
    // words (12 bits), input timing (16 bits).
    const uint32_t length = ((mHeaderBuffer[0] & 0x0F) << 8 | mHeaderBuffer[1]) * 2;
    if (length < mHeaderLength || length > MAT_BURST_BYTES / 2) {
        ALOGE("TrueHDFrameScanner: ERROR - access unit length = %u", length);
        return false;
    }
    mInputTiming = (mHeaderBuffer[2] << 8) | mHeaderBuffer[3];

    if (mHeaderLength == MAJOR_SYNC_HEADER_BYTES && isMajorSync()) {
        // audio_sampling_frequency: 0..2 = 48 kHz x 1, 2, 4; +8 for 44.1 kHz.
        const uint8_t rateBits = (mHeaderBuffer[7] == 0xBA ? mHeaderBuffer[8]
                                                            : mHeaderBuffer[9]) >> 4;
        if ((rateBits & 0x07) > 2) {
            ALOGE("TrueHDFrameScanner: ERROR - unsupported sample rate code %u", rateBits);
            return false;
        }
        const uint32_t samplesPerAccessUnit = 40 << (rateBits & 0x07);
        const uint32_t sampleRate = (rateBits & 0x08) ? 44100 : 48000;
        ALOGI_IF(samplesPerAccessUnit != mSamplesPerAccessUnit || sampleRate != mSampleRate,
                "TrueHD: %u samples per access unit, base rate %u", samplesPerAccessUnit,
                sampleRate);
        mSamplesPerAccessUnit = samplesPerAccessUnit;
        mSampleRate = sampleRate;
    } else if (mSamplesPerAccessUnit == 0) {
        return false;
    }
    mFrameSizeBytes = length;
    return true;
}

}  // namespace android
