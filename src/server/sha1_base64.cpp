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

#include "server/sha1_base64.hpp"

#include <cstring>
#include <vector>

namespace otns {
namespace Server {

namespace {

uint32_t Rol32(uint32_t aValue, unsigned aBits) { return (aValue << aBits) | (aValue >> (32u - aBits)); }

uint32_t ReadU32Be(const uint8_t *aPtr)
{
    return (static_cast<uint32_t>(aPtr[0]) << 24u) | (static_cast<uint32_t>(aPtr[1]) << 16u) |
           (static_cast<uint32_t>(aPtr[2]) << 8u) | static_cast<uint32_t>(aPtr[3]);
}

void WriteU32Be(uint8_t *aPtr, uint32_t aValue)
{
    aPtr[0] = static_cast<uint8_t>((aValue >> 24u) & 0xFFu);
    aPtr[1] = static_cast<uint8_t>((aValue >> 16u) & 0xFFu);
    aPtr[2] = static_cast<uint8_t>((aValue >> 8u) & 0xFFu);
    aPtr[3] = static_cast<uint8_t>(aValue & 0xFFu);
}

} // namespace

std::array<uint8_t, 20> ComputeSha1(const uint8_t *aData, size_t aLength)
{
    uint32_t             h0     = 0x67452301u;
    uint32_t             h1     = 0xEFCDAB89u;
    uint32_t             h2     = 0x98BADCFEu;
    uint32_t             h3     = 0x10325476u;
    uint32_t             h4     = 0xC3D2E1F0u;
    uint64_t             bitLen = static_cast<uint64_t>(aLength) * 8ULL;
    std::vector<uint8_t> msg;

    msg.reserve(((aLength + 9u + 63u) / 64u) * 64u);
    if (aData != nullptr && aLength > 0)
    {
        msg.assign(aData, aData + aLength);
    }
    msg.push_back(0x80u);
    while ((msg.size() % 64u) != 56u)
    {
        msg.push_back(0x00u);
    }
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        msg.push_back(static_cast<uint8_t>((bitLen >> static_cast<unsigned>(shift)) & 0xFFu));
    }

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64u)
    {
        uint32_t w[80];

        for (size_t i = 0; i < 16u; ++i)
        {
            w[i] = ReadU32Be(msg.data() + chunk + (i * 4u));
        }
        for (size_t i = 16u; i < 80u; ++i)
        {
            w[i] = Rol32(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (size_t i = 0; i < 80u; ++i)
        {
            uint32_t f = 0;
            uint32_t k = 0;

            if (i < 20u)
            {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            }
            else if (i < 40u)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            }
            else if (i < 60u)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }

            uint32_t temp = Rol32(a, 5u) + f + e + k + w[i];

            e = d;
            d = c;
            c = Rol32(b, 30u);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<uint8_t, 20> digest = {};

    WriteU32Be(digest.data() + 0, h0);
    WriteU32Be(digest.data() + 4, h1);
    WriteU32Be(digest.data() + 8, h2);
    WriteU32Be(digest.data() + 12, h3);
    WriteU32Be(digest.data() + 16, h4);

    return digest;
}

std::string Base64Encode(const uint8_t *aData, size_t aLength)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;

    out.reserve(((aLength + 2u) / 3u) * 4u);
    for (size_t i = 0; i < aLength; i += 3u)
    {
        uint32_t b0 = aData[i];
        uint32_t b1 = (i + 1u < aLength) ? aData[i + 1u] : 0u;
        uint32_t b2 = (i + 2u < aLength) ? aData[i + 2u] : 0u;
        uint32_t n  = (b0 << 16u) | (b1 << 8u) | b2;

        out.push_back(kAlphabet[(n >> 18u) & 0x3Fu]);
        out.push_back(kAlphabet[(n >> 12u) & 0x3Fu]);
        out.push_back((i + 1u < aLength) ? kAlphabet[(n >> 6u) & 0x3Fu] : '=');
        out.push_back((i + 2u < aLength) ? kAlphabet[n & 0x3Fu] : '=');
    }

    return out;
}

std::string ComputeWebSocketAccept(const std::string &aClientKey)
{
    static constexpr char kWsMagicGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    std::string combined = aClientKey + kWsMagicGuid;
    auto        digest   = ComputeSha1(reinterpret_cast<const uint8_t *>(combined.data()), combined.size());

    return Base64Encode(digest.data(), digest.size());
}

} // namespace Server
} // namespace otns
