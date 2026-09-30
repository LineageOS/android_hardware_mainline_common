/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "AudioSPDIF"
//#define LOG_NDEBUG 0

#include <string.h>

#include <log/log.h>
#include <audio_utils/spdif/FrameScanner.h>

#include "BitFieldParser.h"
#include "DTSHDFrameScanner.h"

namespace android {

// Sync word of the extension substream (SYNCEXTSSH in ETSI TS 102 114).
const uint8_t DTSHDFrameScanner::kExtensionSubstreamSyncBytes[] =
        { 0x64, 0x58, 0x20, 0x25 };

#define DTSHD_SYNC_BYTES                         4
// Bytes needed to read the extension substream size: sync word, then
// UserDefinedBits (8), nExtSSIndex (2), bHeaderSizeType (1),
// nuExtSSHeaderSize (8 or 12) and nuExtSSFsize (16 or 20 bits).
#define DTSHD_EXSS_HEADER_BYTES_NEEDED          10

#define DTSHD_MIN_REPETITION_PERIOD            512

DTSHDFrameScanner::DTSHDFrameScanner(uint32_t rateMultiplier)
 : DTSFrameScanner()
 , mHdRateMultiplier(rateMultiplier)
 , mCoreHeaderLength(mHeaderLength)
 , mIsExtensionSubstream(false)
 , mCoreSeen(false)
{
    mDataType = kSpdifDataTypeDtsTypeIV;
    mRateMultiplier = rateMultiplier;
}

DTSHDFrameScanner::~DTSHDFrameScanner()
{
}

// Like FrameScanner::scan(), but for two sync words: the one of the core and
// the one of the extension substream. The header length depends on which one
// was found.
bool DTSHDFrameScanner::scan(uint8_t byte)
{
    if (mCursor < DTSHD_SYNC_BYTES) {
        mHeaderBuffer[mCursor++] = byte;
        const bool core = memcmp(mHeaderBuffer, kSyncBytes, mCursor) == 0;
        const bool extension =
                memcmp(mHeaderBuffer, kExtensionSubstreamSyncBytes, mCursor) == 0;
        if (!core && !extension) {
            // Not a sync word. The byte may still start the next one.
            mBytesSkipped += mCursor - 1;
            mCursor = 0;
            if (byte == kSyncBytes[0] || byte == kExtensionSubstreamSyncBytes[0]) {
                mHeaderBuffer[mCursor++] = byte;
            } else {
                mBytesSkipped++;
            }
        } else if (mCursor == DTSHD_SYNC_BYTES) {
            mIsExtensionSubstream = extension;
            mHeaderLength = extension ? DTSHD_EXSS_HEADER_BYTES_NEEDED
                                      : mCoreHeaderLength;
        }
        return false;
    }
    mHeaderBuffer[mCursor++] = byte;
    if (mCursor < mHeaderLength) {
        return false;
    }
    mCursor = 0;
    if (!parseHeader()) {
        ALOGE("DTSHDFrameScanner: ERROR - parseHeader() failed.");
        return false;
    }
    return true;
}

bool DTSHDFrameScanner::parseHeader()
{
    if (mIsExtensionSubstream) {
        return parseExtensionSubstreamHeader();
    }
    mCoreSeen = false;
    if (!DTSFrameScanner::parseHeader()) {
        return false;
    }
    // A type IV burst, whatever the core length. The repetition period is
    // one of 512 << n IEC frames; its sub-type n is the data type info.
    mDataType = kSpdifDataTypeDtsTypeIV;
    mRateMultiplier = mHdRateMultiplier;
    const int period = getSampleFramesPerSyncFrame();
    int subType = 0;
    while ((DTSHD_MIN_REPETITION_PERIOD << subType) < period) {
        subType++;
    }
    if ((DTSHD_MIN_REPETITION_PERIOD << subType) != period
            || period > DTSHD_MAX_REPETITION_PERIOD) {
        ALOGE("DTSHDFrameScanner: ERROR - %d frames x %u is not a type IV period",
                mSampleFramesPerSyncFrame, mRateMultiplier);
        return false;
    }
    mDataTypeInfo = subType;
    mCoreSeen = true;
    return true;
}

bool DTSHDFrameScanner::parseExtensionSubstreamHeader()
{
    if (!mCoreSeen) {
        ALOGV("DTSHDFrameScanner: skipping an extension substream without a core");
        return false;
    }
    mCoreSeen = false;
    BitFieldParser parser(&mHeaderBuffer[DTSHD_SYNC_BYTES]);
    (void) /* uint32_t userDefinedBits = */ parser.readBits(8);
    (void) /* uint32_t extSSIndex = */ parser.readBits(2);
    const uint32_t headerSizeType = parser.readBits(1);
    const uint32_t headerSize = parser.readBits(headerSizeType == 0 ? 8 : 12) + 1;
    const uint32_t frameSize = parser.readBits(headerSizeType == 0 ? 16 : 20) + 1;
    if (frameSize < mHeaderLength || headerSize > frameSize) {
        ALOGE("DTSHDFrameScanner: ERROR - extension substream header %u, frame %u bytes",
                headerSize, frameSize);
        return false;
    }
    mFrameSizeBytes = frameSize;
    return true;
}

}  // namespace android
