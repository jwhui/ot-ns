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

#ifndef OTNS_CORE_WPAN_DISSECTOR_HPP_
#define OTNS_CORE_WPAN_DISSECTOR_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#include "rf/types.hpp"

namespace otns {
namespace Core {

enum class MacFrameType : uint8_t
{
    kBeacon  = 0,
    kData    = 1,
    kAck     = 2,
    kCommand = 3,
    kUnknown = 7,
};

enum class MacAddrMode : uint8_t
{
    kNone     = 0,
    kReserved = 1,
    kShort    = 2,
    kExtended = 3,
};

struct MacFrameInfo
{
    bool        IsBroadcast(void) const;
    std::string ToSummaryString(void) const;

    bool          mValid            = false;
    Rf::ChannelId mChannel          = Rf::kMinChannel;
    uint16_t      mFrameControl     = 0;
    MacFrameType  mFrameType        = MacFrameType::kUnknown;
    uint8_t       mFrameVersion     = 0;
    bool          mSecurityEnabled  = false;
    bool          mFramePending     = false;
    bool          mAckRequested     = false;
    bool          mPanIdCompression = false;
    bool          mSeqSuppressed    = false;
    bool          mIePresent        = false;
    uint8_t       mSeq              = 0;
    bool          mHasDstPanId      = false;
    uint16_t      mDstPanId         = 0xFFFF;
    bool          mHasSrcPanId      = false;
    uint16_t      mSrcPanId         = 0xFFFF;
    MacAddrMode   mDstAddrMode      = MacAddrMode::kNone;
    MacAddrMode   mSrcAddrMode      = MacAddrMode::kNone;
    uint16_t      mDstAddrShort     = 0xFFFF;
    uint16_t      mSrcAddrShort     = 0xFFFF;
    uint64_t      mDstAddrExtended  = 0;
    uint64_t      mSrcAddrExtended  = 0;
    uint16_t      mPsduLength       = 0;
    uint16_t      mHeaderLength     = 0;
};

/**
 * Bounds-checked IEEE 802.15.4-2006 / 2015 MAC header dissector.
 */
MacFrameInfo DissectWpanFrame(const uint8_t *aPsdu, size_t aLength, Rf::ChannelId aChannel);

} // namespace Core
} // namespace otns

#endif // OTNS_CORE_WPAN_DISSECTOR_HPP_
