// SPDX-License-Identifier: MIT
// Copyright (c) 2026 MicroHAD contributors
#ifndef MICROHAD_HDLC_RX_ERROR_LOG_HPP
#define MICROHAD_HDLC_RX_ERROR_LOG_HPP

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Error-only snapshot. Pump emits at most one bounded record and advances only
// after the NCP stream accepts it. No allocation or normal-frame byte copying.
template <unsigned Capacity> class HdlcRxErrorLog
{
public:
    HdlcRxErrorLog(void)
        : mSequence(0), mId(0), mDropped(0), mLength(0), mOriginalLength(0), mOffset(0),
          mResidue(0xffff), mError(0), mStage(kIdle)
    {
    }

    void Record(int error, const uint8_t *bytes, uint16_t length, const char *diagnostics = NULL)
    {
        ++mSequence;
        if (mStage != kIdle)
        {
            ++mDropped;
            return; // Retain the earlier snapshot until it has been emitted.
        }
        mId = mSequence;
        mDiagnostics[0] = 0;
        if (diagnostics != NULL) snprintf(mDiagnostics, sizeof(mDiagnostics), "%s", diagnostics);
        mError = error;
        mOriginalLength = length;
        mLength = length < Capacity ? length : Capacity;
        if (mLength != 0) memcpy(mBytes, bytes, mLength);
        mOffset = 0;
        mResidue = 0xffff;
        for (unsigned i = 0; i < mLength; ++i)
        {
            mResidue ^= mBytes[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                mResidue = (mResidue >> 1) ^ ((mResidue & 1) ? 0x8408 : 0);
        }
        mStage = kHeader;
    }

    template <typename Write> void Pump(Write write)
    {
        if (mStage == kIdle) return;
        char line[128];
        int length;
        unsigned chunk = 0;
        if (mStage == kHeader)
        {
            length = snprintf(line, sizeof(line),
                              "Framing error %d: id=%lu len=%u captured=%u residue=%04x\n",
                              mError, static_cast<unsigned long>(mId), mOriginalLength, mLength, mResidue);
        }
        else if (mStage == kStats)
        {
            length = snprintf(line, sizeof(line), "%s", mDiagnostics);
        }
        else if (mStage == kBytes)
        {
            static const char hex[] = "0123456789ABCDEF";
            chunk = mLength - mOffset;
            if (chunk > 32) chunk = 32;
            length = snprintf(line, sizeof(line), "HDLC_RX_FRAME id=%lu offset=%u len=%u hex=",
                              static_cast<unsigned long>(mId), mOffset, chunk);
            for (unsigned i = 0; i < chunk; ++i)
            {
                line[length++] = hex[mBytes[mOffset + i] >> 4];
                line[length++] = hex[mBytes[mOffset + i] & 15];
            }
            line[length++] = '\n';
        }
        else
        {
            length = snprintf(line, sizeof(line), "HDLC_RX_END id=%lu captured=%u dropped_total=%lu\n",
                              static_cast<unsigned long>(mId), mLength, static_cast<unsigned long>(mDropped));
        }
        if (length <= 0 || static_cast<unsigned>(length) >= sizeof(line) || !write(line, length)) return;
        if (mStage == kHeader) mStage = mDiagnostics[0] ? kStats : mLength ? kBytes : kEnd;
        else if (mStage == kStats) mStage = mLength ? kBytes : kEnd;
        else if (mStage == kBytes)
        {
            mOffset += chunk;
            if (mOffset == mLength) mStage = kEnd;
        }
        else mStage = kIdle;
    }

private:
    enum Stage { kIdle, kHeader, kStats, kBytes, kEnd };
    uint32_t mSequence, mId, mDropped;
    uint16_t mLength, mOriginalLength, mOffset, mResidue;
    int mError;
    Stage mStage;
    uint8_t mBytes[Capacity];
    char mDiagnostics[128];
};
#endif
