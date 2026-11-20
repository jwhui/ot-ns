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

#include "protocol/rfsim_v3_codec.hpp"

#include <limits>

namespace otns {
namespace Protocol {

namespace {

void AppendU16Le(std::vector<uint8_t> &aBuf, uint16_t aValue)
{
    aBuf.push_back(static_cast<uint8_t>(aValue & 0xFFu));
    aBuf.push_back(static_cast<uint8_t>((aValue >> 8u) & 0xFFu));
}

void AppendU32Le(std::vector<uint8_t> &aBuf, uint32_t aValue)
{
    aBuf.push_back(static_cast<uint8_t>(aValue & 0xFFu));
    aBuf.push_back(static_cast<uint8_t>((aValue >> 8u) & 0xFFu));
    aBuf.push_back(static_cast<uint8_t>((aValue >> 16u) & 0xFFu));
    aBuf.push_back(static_cast<uint8_t>((aValue >> 24u) & 0xFFu));
}

void AppendU64Le(std::vector<uint8_t> &aBuf, uint64_t aValue)
{
    for (unsigned shift = 0; shift < 64; shift += 8)
    {
        aBuf.push_back(static_cast<uint8_t>((aValue >> shift) & 0xFFu));
    }
}

uint16_t ReadU16Le(const uint8_t *aPtr)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(aPtr[0]) | (static_cast<uint16_t>(aPtr[1]) << 8u));
}

uint32_t ReadU32Le(const uint8_t *aPtr)
{
    return static_cast<uint32_t>(aPtr[0]) | (static_cast<uint32_t>(aPtr[1]) << 8u) |
           (static_cast<uint32_t>(aPtr[2]) << 16u) | (static_cast<uint32_t>(aPtr[3]) << 24u);
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

} // namespace

void Frame::AddBytes(TlvType aType, const uint8_t *aData, size_t aLength)
{
    Tlv tlv;

    tlv.mType = aType;
    if (aData != nullptr && aLength > 0)
    {
        tlv.mValue.assign(aData, aData + aLength);
    }

    mTlvs.push_back(std::move(tlv));
}

void Frame::AddU8(TlvType aType, uint8_t aValue) { AddBytes(aType, &aValue, 1); }

void Frame::AddI8(TlvType aType, int8_t aValue) { AddU8(aType, static_cast<uint8_t>(aValue)); }

void Frame::AddU16(TlvType aType, uint16_t aValue)
{
    std::vector<uint8_t> bytes;

    bytes.reserve(2);
    AppendU16Le(bytes, aValue);
    mTlvs.push_back({aType, std::move(bytes)});
}

void Frame::AddU32(TlvType aType, uint32_t aValue)
{
    std::vector<uint8_t> bytes;

    bytes.reserve(4);
    AppendU32Le(bytes, aValue);
    mTlvs.push_back({aType, std::move(bytes)});
}

void Frame::AddU64(TlvType aType, uint64_t aValue)
{
    std::vector<uint8_t> bytes;

    bytes.reserve(8);
    AppendU64Le(bytes, aValue);
    mTlvs.push_back({aType, std::move(bytes)});
}

void Frame::AddVersion(uint16_t aMajor, uint16_t aMinor)
{
    std::vector<uint8_t> bytes;

    bytes.reserve(4);
    AppendU16Le(bytes, aMajor);
    AppendU16Le(bytes, aMinor);
    mTlvs.push_back({TlvType::kVersion, std::move(bytes)});
}

void Frame::AddText(TlvType aType, const std::string &aText)
{
    AddBytes(aType, reinterpret_cast<const uint8_t *>(aText.data()), aText.size());
}

const Tlv *Frame::FindTlv(TlvType aType) const
{
    for (const auto &tlv : mTlvs)
    {
        if (tlv.mType == aType)
        {
            return &tlv;
        }
    }

    return nullptr;
}

std::optional<uint8_t> Frame::GetU8(TlvType aType) const
{
    const Tlv *tlv = FindTlv(aType);

    if (tlv == nullptr || tlv->mValue.size() != 1)
    {
        return std::nullopt;
    }

    return tlv->mValue[0];
}

std::optional<int8_t> Frame::GetI8(TlvType aType) const
{
    auto value = GetU8(aType);

    if (!value.has_value())
    {
        return std::nullopt;
    }

    return static_cast<int8_t>(*value);
}

std::optional<uint32_t> Frame::GetU32(TlvType aType) const
{
    const Tlv *tlv = FindTlv(aType);

    if (tlv == nullptr || tlv->mValue.size() != 4)
    {
        return std::nullopt;
    }

    return ReadU32Le(tlv->mValue.data());
}

std::optional<uint64_t> Frame::GetU64(TlvType aType) const
{
    const Tlv *tlv = FindTlv(aType);

    if (tlv == nullptr || tlv->mValue.size() != 8)
    {
        return std::nullopt;
    }

    return ReadU64Le(tlv->mValue.data());
}

std::optional<std::string> Frame::GetText(TlvType aType) const
{
    const Tlv *tlv = FindTlv(aType);

    if (tlv == nullptr)
    {
        return std::nullopt;
    }

    return std::string(reinterpret_cast<const char *>(tlv->mValue.data()), tlv->mValue.size());
}

bool EncodeFrame(const Frame &aFrame, std::vector<uint8_t> &aOut)
{
    size_t payloadLen = 0;

    for (const auto &tlv : aFrame.mTlvs)
    {
        if (tlv.mValue.size() > std::numeric_limits<uint16_t>::max())
        {
            return false;
        }

        size_t tlvBytes = kTlvHeaderSize + tlv.mValue.size();

        if (tlvBytes > kMaxPayloadBytes || payloadLen > kMaxPayloadBytes - tlvBytes)
        {
            return false;
        }

        payloadLen += tlvBytes;
    }

    aOut.reserve(aOut.size() + kFrameHeaderSize + payloadLen);
    AppendU32Le(aOut, static_cast<uint32_t>(payloadLen));
    AppendU16Le(aOut, static_cast<uint16_t>(aFrame.mType));
    AppendU16Le(aOut, aFrame.mFlags);
    AppendU64Le(aOut, aFrame.mTimestampUs);

    for (const auto &tlv : aFrame.mTlvs)
    {
        AppendU16Le(aOut, static_cast<uint16_t>(tlv.mType));
        AppendU16Le(aOut, static_cast<uint16_t>(tlv.mValue.size()));
        aOut.insert(aOut.end(), tlv.mValue.begin(), tlv.mValue.end());
    }

    return true;
}

DecodeResult DecodeFrame(const uint8_t *aData, size_t aLength)
{
    DecodeResult result;

    if (aData == nullptr || aLength < kFrameHeaderSize)
    {
        result.mStatus = DecodeStatus::kNeedMoreData;
        return result;
    }

    uint32_t payloadLen = ReadU32Le(aData);

    if (payloadLen > kMaxPayloadBytes)
    {
        result.mStatus = DecodeStatus::kPayloadTooLarge;
        return result;
    }

    size_t totalLen = kFrameHeaderSize + static_cast<size_t>(payloadLen);

    if (aLength < totalLen)
    {
        result.mStatus = DecodeStatus::kNeedMoreData;
        return result;
    }

    result.mFrame.mType        = static_cast<MsgType>(ReadU16Le(aData + 4));
    result.mFrame.mFlags       = ReadU16Le(aData + 6);
    result.mFrame.mTimestampUs = ReadU64Le(aData + 8);

    const uint8_t *payload = aData + kFrameHeaderSize;
    size_t         offset  = 0;

    while (offset < payloadLen)
    {
        if (payloadLen - offset < kTlvHeaderSize)
        {
            result.mStatus = DecodeStatus::kMalformedTlv;
            return result;
        }

        TlvType  tlvType = static_cast<TlvType>(ReadU16Le(payload + offset));
        uint16_t tlvLen  = ReadU16Le(payload + offset + 2);

        offset += kTlvHeaderSize;

        if (payloadLen - offset < tlvLen)
        {
            result.mStatus = DecodeStatus::kMalformedTlv;
            return result;
        }

        Tlv tlv;

        tlv.mType = tlvType;
        if (tlvLen > 0)
        {
            tlv.mValue.assign(payload + offset, payload + offset + tlvLen);
        }

        result.mFrame.mTlvs.push_back(std::move(tlv));
        offset += tlvLen;
    }

    result.mStatus        = DecodeStatus::kOk;
    result.mBytesConsumed = totalLen;

    return result;
}

} // namespace Protocol
} // namespace otns
