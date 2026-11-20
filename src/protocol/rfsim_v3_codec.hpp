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

#ifndef OTNS_PROTOCOL_RFSIM_V3_CODEC_HPP_
#define OTNS_PROTOCOL_RFSIM_V3_CODEC_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace otns {
namespace Protocol {

static constexpr std::array<uint8_t, 4> kProtocolMagic   = {'O', 'T', 'R', 'F'};
static constexpr uint16_t               kProtocolMajor   = 3;
static constexpr uint16_t               kProtocolMinor   = 0;
static constexpr size_t                 kFrameHeaderSize = 16;
static constexpr size_t                 kTlvHeaderSize   = 4;
static constexpr uint32_t               kMaxPayloadBytes = 65536;

static constexpr uint16_t kFlagRelativeDelay = 0x0001;
static constexpr uint16_t kFlagCancel        = 0x0002;
static constexpr uint16_t kFlagAckRequested  = 0x0004;
static constexpr uint16_t kFlagCsmaCa        = 0x0008;
static constexpr uint16_t kFlagResponse      = 0x0010;

enum class MsgType : uint16_t
{
    kHello         = 0x0001,
    kHelloAck      = 0x0002,
    kShutdown      = 0x0003,
    kAlarmSet      = 0x0010,
    kAlarmFired    = 0x0011,
    kStepDone      = 0x0012,
    kRadioState    = 0x0020,
    kTxStart       = 0x0021,
    kTxDone        = 0x0022,
    kRxStart       = 0x0023,
    kRxDone        = 0x0024,
    kEnergyScan    = 0x0025,
    kRadioParam    = 0x0026,
    kCliData       = 0x0030,
    kLogRecord     = 0x0031,
    kStatusPush    = 0x0032,
    kTrelPacket    = 0x0040,
    kInfraIfPacket = 0x0041,
    kHostSocket    = 0x0042,
    kTrelPeer      = 0x0043,
    kInfraIfState  = 0x0044,
};

enum class TlvType : uint16_t
{
    kMagic           = 0x0001,
    kVersion         = 0x0002,
    kNodeId          = 0x0003,
    kSeed            = 0x0004,
    kChannel         = 0x0005,
    kPowerDbm        = 0x0006,
    kSensitivityDbm  = 0x0007,
    kDurationUs      = 0x0008,
    kErrorCode       = 0x0009,
    kMsgId           = 0x000A,
    kRadioState      = 0x000B,
    kPsdu            = 0x000C,
    kLqi             = 0x000D,
    kText            = 0x000E,
    kExtAddr         = 0x000F,
    kParamKv         = 0x0010,
    kCapabilities    = 0x0011,
    kNetAddr         = 0x0012,
    kPayload         = 0x0013,
    kCcaThresholdDbm = 0x0014,
    kStatusInfo      = 0x0015,
    kAckPsdu         = 0x0016,
};

struct VersionValue
{
    uint16_t mMajor = 0;
    uint16_t mMinor = 0;
};

struct RadioStateValue
{
    uint8_t mState    = 0;
    uint8_t mSubState = 0;
};

struct ParamKvValue
{
    uint16_t mParamId = 0;
    int32_t  mValue   = 0;
};

struct NetAddrValue
{
    uint16_t                mSrcPort = 0;
    uint16_t                mDstPort = 0;
    std::array<uint8_t, 16> mSrcIp6  = {};
    std::array<uint8_t, 16> mDstIp6  = {};
};

struct StatusInfoValue
{
    uint8_t                mRole        = 0;
    uint16_t               mRloc16      = 0xFFFE;
    uint32_t               mPartitionId = 0;
    std::array<uint8_t, 8> mExtAddr     = {};
};

struct Tlv
{
    TlvType              mType;
    std::vector<uint8_t> mValue;
};

struct Frame
{
    void AddBytes(TlvType aType, const uint8_t *aData, size_t aLength);
    void AddU8(TlvType aType, uint8_t aValue);
    void AddI8(TlvType aType, int8_t aValue);
    void AddU16(TlvType aType, uint16_t aValue);
    void AddU32(TlvType aType, uint32_t aValue);
    void AddU64(TlvType aType, uint64_t aValue);
    void AddVersion(uint16_t aMajor, uint16_t aMinor);
    void AddRadioState(uint8_t aState, uint8_t aSubState);
    void AddParamKv(uint16_t aParamId, int32_t aValue);
    void AddNetAddr(const NetAddrValue &aAddr);
    void AddStatusInfo(const StatusInfoValue &aInfo);
    void AddText(TlvType aType, const std::string &aText);

    const Tlv                     *FindTlv(TlvType aType) const;
    std::optional<uint8_t>         GetU8(TlvType aType) const;
    std::optional<int8_t>          GetI8(TlvType aType) const;
    std::optional<uint16_t>        GetU16(TlvType aType) const;
    std::optional<uint32_t>        GetU32(TlvType aType) const;
    std::optional<uint64_t>        GetU64(TlvType aType) const;
    std::optional<VersionValue>    GetVersion(void) const;
    std::optional<RadioStateValue> GetRadioState(void) const;
    std::optional<ParamKvValue>    GetParamKv(void) const;
    std::optional<NetAddrValue>    GetNetAddr(void) const;
    std::optional<StatusInfoValue> GetStatusInfo(void) const;
    std::optional<std::string>     GetText(TlvType aType) const;

    MsgType          mType        = MsgType::kHello;
    uint16_t         mFlags       = 0;
    uint64_t         mTimestampUs = 0;
    std::vector<Tlv> mTlvs;
};

enum class DecodeStatus : uint8_t
{
    kOk,
    kNeedMoreData,
    kPayloadTooLarge,
    kMalformedTlv,
};

struct DecodeResult
{
    DecodeStatus mStatus        = DecodeStatus::kNeedMoreData;
    size_t       mBytesConsumed = 0;
    Frame        mFrame;
};

bool         EncodeFrame(const Frame &aFrame, std::vector<uint8_t> &aOut);
DecodeResult DecodeFrame(const uint8_t *aData, size_t aLength);

} // namespace Protocol
} // namespace otns

#endif // OTNS_PROTOCOL_RFSIM_V3_CODEC_HPP_
