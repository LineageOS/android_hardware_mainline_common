/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

// Dolby TrueHD in MAT frames (IEC 61937-9, data type 22).
//
// A MAT data burst is the IEC 61937 preamble, a MAT frame of MAT_FRAME_BYTES
// and zero stuffing up to MAT_BURST_BYTES. The MAT frame carries a start code
// at its beginning, a middle code at a fixed offset and an end code at its
// end; the TrueHD access units fill the space in between, split around the
// codes where necessary. A MAT stream position advances by
// MAT_BYTES_PER_40_SAMPLES per 40 samples of audio, so every access unit is
// placed at the position its input timing asks for, with zero padding before
// it. The codes, the preamble and the stuffing count as part of the stream,
// which is why 24 access units of 40 samples fill exactly one burst.

#define LOG_TAG "AudioSPDIF"
//#define LOG_NDEBUG 0

#include <stdint.h>
#include <string.h>

#include <log/log.h>
#include <audio_utils/spdif/SPDIFEncoder.h>

#include "TrueHDFrameScanner.h"

namespace android {

namespace {

#define MAT_FRAME_BYTES     61424

const uint8_t kMatStartCode[] = {
    0x07, 0x9E, 0x00, 0x03, 0x84, 0x01, 0x01, 0x01, 0x80, 0x00,
    0x56, 0xA5, 0x3B, 0xF4, 0x81, 0x83, 0x49, 0x80, 0x77, 0xE0,
};
const uint8_t kMatMiddleCode[] = {
    0xC3, 0xC1, 0x42, 0x49, 0x3B, 0xFA, 0x82, 0x83, 0x49, 0x80, 0x77, 0xE0,
};
const uint8_t kMatEndCode[] = {
    0xC3, 0xC2, 0xC0, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x97, 0x11, 0x00, 0x00,
};

struct MatCode {
    size_t offset;  // in the MAT frame
    const uint8_t *bytes;
    size_t size;
};

const MatCode kMatCodes[] = {
    { 0, kMatStartCode, sizeof(kMatStartCode) },
    { 30708, kMatMiddleCode, sizeof(kMatMiddleCode) },
    { MAT_FRAME_BYTES - sizeof(kMatEndCode), kMatEndCode, sizeof(kMatEndCode) },
};
const size_t kNumMatCodes = sizeof(kMatCodes) / sizeof(kMatCodes[0]);

}  // namespace

void SPDIFEncoder::resetMat()
{
    mMatFill = 0;
    mMatNextCode = 0;
    mMatPosition = 0;
    mMatUnitPosition = 0;
    mMatUnitTiming = 0;
    mMatHaveUnit = false;
}

// Writes the codes that are due at the current position. The end code
// completes the burst, and the start code of the next MAT frame follows it
// right away.
void SPDIFEncoder::insertDueMatCodes()
{
    while (mMatFill == kMatCodes[mMatNextCode].offset) {
        const MatCode &code = kMatCodes[mMatNextCode];
        if (mMatNextCode == 0) {
            startDataBurst();
        }
        writeBurstBufferBytes(code.bytes, code.size);
        mMatFill += code.size;
        mMatPosition += code.size;
        if (++mMatNextCode == kNumMatCodes) {
            sendBurstBuffer();
            clearBurstBuffer();
            // The stuffing of this burst and the preamble of the next one.
            mMatPosition += MAT_BURST_BYTES - MAT_FRAME_BYTES;
            mMatFill = 0;
            mMatNextCode = 0;
        }
    }
}

// Appends access unit data, or zero padding when `data` is NULL.
void SPDIFEncoder::appendMat(const uint8_t *data, size_t numBytes)
{
    while (numBytes > 0) {
        insertDueMatCodes();
        size_t chunk = kMatCodes[mMatNextCode].offset - mMatFill;
        if (chunk > numBytes) {
            chunk = numBytes;
        }
        if (data != NULL) {
            writeBurstBufferBytes(data, chunk);
            data += chunk;
        } else {
            // The burst buffer is cleared, so padding only moves the cursor.
            mByteCursor += chunk;
        }
        mMatFill += chunk;
        mMatPosition += chunk;
        numBytes -= chunk;
    }
    insertDueMatCodes();
}

// Pads the MAT stream up to where the access unit found by the scanner
// belongs, then writes its header.
void SPDIFEncoder::startMatAccessUnit()
{
    const TrueHDFrameScanner *scanner = static_cast<const TrueHDFrameScanner *>(mFramer);
    const uint16_t timing = scanner->getInputTiming();
    int64_t unitPosition = mMatPosition;
    if (mMatHaveUnit) {
        const uint16_t samples = timing - mMatUnitTiming;
        const int64_t target = mMatUnitPosition + (int64_t) samples
                * (MAT_BYTES_PER_40_SAMPLES * 40 / scanner->getSamplesPerAccessUnit());
        const int64_t padding = target - mMatPosition;
        if (padding >= 0 && padding < MAT_FRAME_BYTES / 2) {
            appendMat(NULL, (size_t) padding);
            // Codes written while padding may overshoot the target; keep the
            // nominal position so that the next padding makes up for it.
            unitPosition = target;
        } else {
            ALOGW("SPDIFEncoder: TrueHD timing jumps by %u samples, padding = %lld",
                    samples, (long long) padding);
            unitPosition = mMatPosition;
        }
    }
    mMatUnitPosition = unitPosition;
    mMatUnitTiming = timing;
    mMatHaveUnit = true;
    appendMat(mFramer->getHeaderAddress(), mFramer->getHeaderSizeBytes());
}

ssize_t SPDIFEncoder::writeMat(const void *buffer, size_t numBytes)
{
    size_t bytesLeft = numBytes;
    const uint8_t *data = (const uint8_t *)buffer;
    while (bytesLeft > 0) {
        if (mScanning) {
            if (mFramer->scan(*data)) {
                startMatAccessUnit();
                mPayloadBytesPending =
                        mFramer->getFrameSizeBytes() - mFramer->getHeaderSizeBytes();
                mScanning = mPayloadBytesPending == 0;
            }
            data++;
            bytesLeft--;
        } else {
            size_t bytesToWrite = bytesLeft;
            if (bytesToWrite > mPayloadBytesPending) {
                bytesToWrite = mPayloadBytesPending;
            }
            appendMat(data, bytesToWrite);
            data += bytesToWrite;
            bytesLeft -= bytesToWrite;
            mPayloadBytesPending -= bytesToWrite;
            if (mPayloadBytesPending == 0) {
                mScanning = true;
            }
        }
    }
    return numBytes;
}

}  // namespace android
