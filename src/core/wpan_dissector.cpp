/*
 *  Copyright (c) 2026, The OTNS Authors.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/wpan_dissector.hpp"

#include <cstdio>

namespace otns {
namespace Core {

namespace {

uint16_t ReadU16Le(const uint8_t *aPtr)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(aPtr[0]) | (static_cast<uint16_t>(aPtr[1]) << 8u));
}

uint64_t ReadU64Le(const uint8_t *aPtr)
{
    uint64_t value = 0;

    for (unsigned i = 0; i < 8; ++i)
    {
        value |= static_cast<uint64_t>(aPtr[i]) << (i * 8u);
    }

    return value;
}

bool HasDestPanIdField(uint8_t aVersion, MacAddrMode aDstMode, MacAddrMode aSrcMode, bool aPanIdComp)
{
    if (aVersion <= 1)
    {
        return aDstMode != MacAddrMode::kNone;
    }

    if (aDstMode != MacAddrMode::kNone && aSrcMode != MacAddrMode::kNone)
    {
        if (aDstMode == MacAddrMode::kExtended && aSrcMode == MacAddrMode::kExtended)
        {
            return !aPanIdComp;
        }
        return true;
    }

    if (aSrcMode == MacAddrMode::kNone && aDstMode != MacAddrMode::kNone)
    {
        return !aPanIdComp;
    }

    if (aSrcMode == MacAddrMode::kNone && aDstMode == MacAddrMode::kNone)
    {
        return aPanIdComp;
    }

    return false;
}

bool HasSourcePanIdField(uint8_t aVersion, MacAddrMode aDstMode, MacAddrMode aSrcMode, bool aPanIdComp)
{
    if (aSrcMode == MacAddrMode::kNone)
    {
        return false;
    }

    if (aVersion <= 1)
    {
        return !aPanIdComp;
    }

    if (aDstMode == MacAddrMode::kExtended && aSrcMode == MacAddrMode::kExtended)
    {
        return false;
    }

    if (aDstMode != MacAddrMode::kNone)
    {
        return !aPanIdComp;
    }

    return !aPanIdComp;
}

} // namespace

bool MacFrameInfo::IsBroadcast(void) const
{
    return (mDstAddrMode == MacAddrMode::kShort && mDstAddrShort == 0xFFFF) || (mDstAddrMode == MacAddrMode::kNone);
}

std::string MacFrameInfo::ToSummaryString(void) const
{
    char buf[128];

    if (!mValid)
    {
        return "INVALID";
    }

    if (mFrameType == MacFrameType::kAck)
    {
        std::snprintf(buf, sizeof(buf), "ACK,FC:0x%04x,Seq:%u", mFrameControl, static_cast<unsigned>(mSeq));
        return buf;
    }

    if (mDstAddrMode == MacAddrMode::kShort)
    {
        std::snprintf(buf, sizeof(buf), "MAC,FC:0x%04x,Seq:%u,Dst:%04x", mFrameControl, static_cast<unsigned>(mSeq),
                      static_cast<unsigned>(mDstAddrShort));
    }
    else if (mDstAddrMode == MacAddrMode::kExtended)
    {
        std::snprintf(buf, sizeof(buf), "MAC,FC:0x%04x,Seq:%u,Dst:%016llx", mFrameControl, static_cast<unsigned>(mSeq),
                      static_cast<unsigned long long>(mDstAddrExtended));
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "MAC,FC:0x%04x,Seq:%u,Dst:-", mFrameControl, static_cast<unsigned>(mSeq));
    }

    return buf;
}

MacFrameInfo DissectWpanFrame(const uint8_t *aPsdu, size_t aLength, Rf::ChannelId aChannel)
{
    MacFrameInfo info;

    info.mChannel    = aChannel;
    info.mPsduLength = static_cast<uint16_t>(aLength);

    // Minimum valid 802.15.4 frame needs at least 2-byte Frame Control + 2-byte FCS (4 bytes).
    if (aPsdu == nullptr || aLength < 4 || aLength > 127)
    {
        return info;
    }

    uint16_t fc         = ReadU16Le(aPsdu);
    uint8_t  rawType    = static_cast<uint8_t>(fc & 0x0007u);
    size_t   payloadEnd = aLength - 2u; // Exclude 2-byte FCS
    size_t   offset     = 2;

    info.mFrameControl     = fc;
    info.mFrameType        = (rawType <= 3) ? static_cast<MacFrameType>(rawType) : MacFrameType::kUnknown;
    info.mSecurityEnabled  = (fc & 0x0008u) != 0;
    info.mFramePending     = (fc & 0x0010u) != 0;
    info.mAckRequested     = (fc & 0x0020u) != 0;
    info.mPanIdCompression = (fc & 0x0040u) != 0;
    info.mSeqSuppressed    = (fc & 0x0100u) != 0;
    info.mIePresent        = (fc & 0x0200u) != 0;
    info.mDstAddrMode      = static_cast<MacAddrMode>((fc & 0x0C00u) >> 10u);
    info.mFrameVersion     = static_cast<uint8_t>((fc & 0x3000u) >> 12u);
    info.mSrcAddrMode      = static_cast<MacAddrMode>((fc & 0xC000u) >> 14u);

    if (info.mDstAddrMode == MacAddrMode::kReserved || info.mSrcAddrMode == MacAddrMode::kReserved)
    {
        return info;
    }

    if (info.mFrameType == MacFrameType::kUnknown)
    {
        info.mHeaderLength = static_cast<uint16_t>(offset);
        info.mValid        = true;
        return info;
    }

    if (!info.mSeqSuppressed)
    {
        if (offset + 1 > payloadEnd)
        {
            return info;
        }
        info.mSeq = aPsdu[offset];
        offset += 1;
    }

    if (HasDestPanIdField(info.mFrameVersion, info.mDstAddrMode, info.mSrcAddrMode, info.mPanIdCompression))
    {
        if (offset + 2 > payloadEnd)
        {
            return info;
        }
        info.mHasDstPanId = true;
        info.mDstPanId    = ReadU16Le(aPsdu + offset);
        offset += 2;
    }

    if (info.mDstAddrMode == MacAddrMode::kShort)
    {
        if (offset + 2 > payloadEnd)
        {
            return info;
        }
        info.mDstAddrShort = ReadU16Le(aPsdu + offset);
        offset += 2;
    }
    else if (info.mDstAddrMode == MacAddrMode::kExtended)
    {
        if (offset + 8 > payloadEnd)
        {
            return info;
        }
        info.mDstAddrExtended = ReadU64Le(aPsdu + offset);
        offset += 8;
    }

    if (HasSourcePanIdField(info.mFrameVersion, info.mDstAddrMode, info.mSrcAddrMode, info.mPanIdCompression))
    {
        if (offset + 2 > payloadEnd)
        {
            return info;
        }
        info.mHasSrcPanId = true;
        info.mSrcPanId    = ReadU16Le(aPsdu + offset);
        offset += 2;
    }

    if (info.mSrcAddrMode == MacAddrMode::kShort)
    {
        if (offset + 2 > payloadEnd)
        {
            return info;
        }
        info.mSrcAddrShort = ReadU16Le(aPsdu + offset);
        offset += 2;
    }
    else if (info.mSrcAddrMode == MacAddrMode::kExtended)
    {
        if (offset + 8 > payloadEnd)
        {
            return info;
        }
        info.mSrcAddrExtended = ReadU64Le(aPsdu + offset);
        offset += 8;
    }

    info.mHeaderLength = static_cast<uint16_t>(offset);
    info.mValid        = true;

    return info;
}

} // namespace Core
} // namespace otns
