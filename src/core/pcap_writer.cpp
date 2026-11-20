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

#include "core/pcap_writer.hpp"

#include <cstring>

#include "core/path_security.hpp"

namespace otns {
namespace Core {

namespace {

static constexpr uint32_t kPcapMagicNumber  = 0xa1b2c3d4u;
static constexpr uint16_t kPcapVersionMajor = 2;
static constexpr uint16_t kPcapVersionMinor = 4;
static constexpr uint32_t kPcapSnapLen      = 256;

static constexpr uint16_t kTapHeaderSize       = 36; // 4B header + 4 aligned TLVs (8B each)
static constexpr uint16_t kTapTlvFcsType       = 0;
static constexpr uint16_t kTapTlvRss           = 1;
static constexpr uint16_t kTapTlvChannelAssign = 3;
static constexpr uint16_t kTapTlvLqi           = 10;

void PutU16Le(uint8_t *aBuf, uint16_t aVal)
{
    aBuf[0] = static_cast<uint8_t>(aVal & 0xFFu);
    aBuf[1] = static_cast<uint8_t>((aVal >> 8u) & 0xFFu);
}

void PutU32Le(uint8_t *aBuf, uint32_t aVal)
{
    aBuf[0] = static_cast<uint8_t>(aVal & 0xFFu);
    aBuf[1] = static_cast<uint8_t>((aVal >> 8u) & 0xFFu);
    aBuf[2] = static_cast<uint8_t>((aVal >> 16u) & 0xFFu);
    aBuf[3] = static_cast<uint8_t>((aVal >> 24u) & 0xFFu);
}

void WriteTapTlv(uint8_t *aBuf, size_t &aOffset, uint16_t aType, const uint8_t *aData, uint16_t aLen)
{
    uint16_t paddedLen = static_cast<uint16_t>((aLen + 3u) & ~3u);

    PutU16Le(aBuf + aOffset, aType);
    PutU16Le(aBuf + aOffset + 2, aLen);
    std::memset(aBuf + aOffset + 4, 0, paddedLen);
    if (aData != nullptr && aLen > 0)
    {
        std::memcpy(aBuf + aOffset + 4, aData, aLen);
    }
    aOffset += 4u + paddedLen;
}

} // namespace

PcapWriter::~PcapWriter(void) { Close(); }

bool PcapWriter::OpenContained(const std::filesystem::path &aBaseDir,
                               const std::string           &aRequestedPath,
                               PcapFormat                   aFormat)
{
    std::filesystem::path resolved;

    if (!ResolveContainedPath(aBaseDir, aRequestedPath, resolved))
    {
        return false;
    }

    return OpenDirect(resolved, aFormat);
}

bool PcapWriter::OpenDirect(const std::filesystem::path &aPath, PcapFormat aFormat)
{
    Close();

    std::error_code       ec;
    std::filesystem::path parent = aPath.parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            return false;
        }
    }

    mFile = std::fopen(aPath.c_str(), "wb");
    if (mFile == nullptr)
    {
        return false;
    }

    mFormat     = aFormat;
    mFrameCount = 0;

    if (!WriteGlobalHeader())
    {
        Close();
        return false;
    }

    return true;
}

void PcapWriter::Close(void)
{
    if (mFile != nullptr)
    {
        std::fflush(mFile);
        std::fclose(mFile);
        mFile = nullptr;
    }
}

bool PcapWriter::WriteGlobalHeader(void)
{
    uint8_t hdr[24];

    std::memset(hdr, 0, sizeof(hdr));
    PutU32Le(hdr + 0, kPcapMagicNumber);
    PutU16Le(hdr + 4, kPcapVersionMajor);
    PutU16Le(hdr + 6, kPcapVersionMinor);
    PutU32Le(hdr + 16, kPcapSnapLen);
    PutU32Le(hdr + 20, static_cast<uint32_t>(mFormat));

    return (std::fwrite(hdr, 1, sizeof(hdr), mFile) == sizeof(hdr)) && (std::fflush(mFile) == 0);
}

bool PcapWriter::AppendFrame(const RawFrame &aFrame)
{
    if (mFile == nullptr || aFrame.mPsdu.empty() || aFrame.mPsdu.size() > kPcapSnapLen)
    {
        return false;
    }

    uint8_t  pktHdr[16];
    uint32_t sec      = static_cast<uint32_t>(aFrame.mTimestampUs / 1000000ULL);
    uint32_t usec     = static_cast<uint32_t>(aFrame.mTimestampUs % 1000000ULL);
    uint32_t tapBytes = (mFormat == PcapFormat::kWpanTap) ? kTapHeaderSize : 0u;
    uint32_t capLen   = static_cast<uint32_t>(aFrame.mPsdu.size()) + tapBytes;

    PutU32Le(pktHdr + 0, sec);
    PutU32Le(pktHdr + 4, usec);
    PutU32Le(pktHdr + 8, capLen);
    PutU32Le(pktHdr + 12, capLen);

    if (std::fwrite(pktHdr, 1, sizeof(pktHdr), mFile) != sizeof(pktHdr))
    {
        return false;
    }

    if (mFormat == PcapFormat::kWpanTap)
    {
        uint8_t  tapHdr[kTapHeaderSize];
        size_t   offset  = 0;
        uint8_t  fcsType = 1; // 16-bit CRC FCS
        float    rssF    = static_cast<float>(aFrame.mRssiDbm);
        uint32_t rssBits = 0;
        uint8_t  rssBuf[4];
        uint8_t  chanBuf[3];
        uint8_t  lqi = aFrame.mLqi;

        std::memset(tapHdr, 0, sizeof(tapHdr));
        tapHdr[0] = 0; // version
        tapHdr[1] = 0; // reserved
        PutU16Le(tapHdr + 2, kTapHeaderSize);
        offset = 4;

        WriteTapTlv(tapHdr, offset, kTapTlvFcsType, &fcsType, 1);

        std::memcpy(&rssBits, &rssF, sizeof(rssBits));
        PutU32Le(rssBuf, rssBits);
        WriteTapTlv(tapHdr, offset, kTapTlvRss, rssBuf, 4);

        PutU16Le(chanBuf, static_cast<uint16_t>(aFrame.mChannel));
        chanBuf[2] = 0; // channel page 0
        WriteTapTlv(tapHdr, offset, kTapTlvChannelAssign, chanBuf, 3);

        WriteTapTlv(tapHdr, offset, kTapTlvLqi, &lqi, 1);

        if (std::fwrite(tapHdr, 1, sizeof(tapHdr), mFile) != sizeof(tapHdr))
        {
            return false;
        }
    }

    if (std::fwrite(aFrame.mPsdu.data(), 1, aFrame.mPsdu.size(), mFile) != aFrame.mPsdu.size())
    {
        return false;
    }

    mFrameCount += 1;

    return true;
}

bool PcapWriter::Flush(void) { return (mFile != nullptr) && (std::fflush(mFile) == 0); }

} // namespace Core
} // namespace otns
