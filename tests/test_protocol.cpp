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

#include <vector>

#include "doctest/doctest.h"
#include "protocol/rfsim_v3_codec.hpp"

using namespace otns::Protocol;

TEST_CASE("rfsim v3 frame encode and decode round-trip")
{
    Frame                tx;
    std::vector<uint8_t> wire;

    tx.mType        = MsgType::kHello;
    tx.mFlags       = kFlagRelativeDelay;
    tx.mTimestampUs = 123456789ULL;
    tx.AddBytes(TlvType::kMagic, kProtocolMagic.data(), kProtocolMagic.size());
    tx.AddVersion(kProtocolMajor, kProtocolMinor);
    tx.AddU32(TlvType::kNodeId, 7);
    tx.AddI8(TlvType::kPowerDbm, -12);
    tx.AddText(TlvType::kText, "matter-light-1");

    CHECK(EncodeFrame(tx, wire));
    if (wire.size() <= 4)
    {
        return;
    }

    // Partial header/payload returns kNeedMoreData.
    CHECK(DecodeFrame(wire.data(), 4).mStatus == DecodeStatus::kNeedMoreData);
    CHECK(DecodeFrame(wire.data(), wire.size() - 1).mStatus == DecodeStatus::kNeedMoreData);

    auto res = DecodeFrame(wire.data(), wire.size());

    CHECK(res.mStatus == DecodeStatus::kOk);
    if (res.mStatus == DecodeStatus::kOk)
    {
        CHECK(res.mBytesConsumed == wire.size());
        CHECK(res.mFrame.mType == MsgType::kHello);
        CHECK(res.mFrame.mFlags == kFlagRelativeDelay);
        CHECK(res.mFrame.mTimestampUs == 123456789ULL);
        CHECK(res.mFrame.GetU32(TlvType::kNodeId) == 7u);
        CHECK(res.mFrame.GetI8(TlvType::kPowerDbm) == -12);
        CHECK(res.mFrame.GetText(TlvType::kText) == "matter-light-1");
    }
}

TEST_CASE("rfsim v3 decoder enforces strict security bounds")
{
    // Null pointer.
    CHECK(DecodeFrame(nullptr, 100).mStatus == DecodeStatus::kNeedMoreData);

    // Oversized payload_len (0xFFFFFFFF) rejected without allocation or overflow.
    std::vector<uint8_t> badLen(kFrameHeaderSize, 0);

    badLen[0] = 0xFF;
    badLen[1] = 0xFF;
    badLen[2] = 0xFF;
    badLen[3] = 0xFF;
    CHECK(DecodeFrame(badLen.data(), badLen.size()).mStatus == DecodeStatus::kPayloadTooLarge);

    // Payload len = kMaxPayloadBytes + 1 rejected.
    uint32_t over = kMaxPayloadBytes + 1;

    badLen[0] = static_cast<uint8_t>(over & 0xFFu);
    badLen[1] = static_cast<uint8_t>((over >> 8u) & 0xFFu);
    badLen[2] = static_cast<uint8_t>((over >> 16u) & 0xFFu);
    badLen[3] = static_cast<uint8_t>((over >> 24u) & 0xFFu);
    CHECK(DecodeFrame(badLen.data(), badLen.size()).mStatus == DecodeStatus::kPayloadTooLarge);

    // Truncated TLV header inside payload (2 bytes payload < 4-byte TLV header).
    std::vector<uint8_t> truncTlv(kFrameHeaderSize + 2, 0);

    truncTlv[0] = 2;
    CHECK(DecodeFrame(truncTlv.data(), truncTlv.size()).mStatus == DecodeStatus::kMalformedTlv);

    // TLV length claiming 10 bytes when only 1 byte remains.
    std::vector<uint8_t> overrunTlv(kFrameHeaderSize + 5, 0);

    overrunTlv[0]                    = 5;  // payload_len = 5
    overrunTlv[kFrameHeaderSize]     = 3;  // tlv_type = 3
    overrunTlv[kFrameHeaderSize + 2] = 10; // tlv_len = 10 > 1 remaining byte
    CHECK(DecodeFrame(overrunTlv.data(), overrunTlv.size()).mStatus == DecodeStatus::kMalformedTlv);
}
