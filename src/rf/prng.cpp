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

#include "rf/prng.hpp"

#include <cmath>
#include <limits>

namespace otns {
namespace Rf {

namespace {

static constexpr double kTwoPi = 6.283185307179586476925286766559;

uint64_t SplitMix64(uint64_t &aState)
{
    uint64_t z = (aState += 0x9e3779b97f4a7c15ULL);

    z = (z ^ (z >> 30u)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27u)) * 0x94d049bb133111ebULL;

    return z ^ (z >> 31u);
}

uint64_t Rotl(uint64_t aValue, unsigned aShift) { return (aValue << aShift) | (aValue >> (64u - aShift)); }

} // namespace

Prng::Prng(uint64_t aSeed) { Seed(aSeed); }

void Prng::Seed(uint64_t aSeed)
{
    uint64_t state = aSeed;

    for (uint64_t &word : mState)
    {
        word = SplitMix64(state);
    }

    mHasSpare   = false;
    mSpareGauss = 0.0;
}

uint64_t Prng::NextU64(void)
{
    uint64_t result = Rotl(mState[1] * 5ULL, 7u) * 9ULL;
    uint64_t t      = mState[1] << 17u;

    mState[2] ^= mState[0];
    mState[3] ^= mState[1];
    mState[1] ^= mState[2];
    mState[0] ^= mState[3];
    mState[2] ^= t;
    mState[3] = Rotl(mState[3], 45u);

    return result;
}

uint32_t Prng::NextU32(void) { return static_cast<uint32_t>(NextU64() >> 32u); }

double Prng::NextUnitDouble(void)
{
    // 53 bits of precision in [0.0, 1.0)
    return static_cast<double>(NextU64() >> 11u) * (1.0 / 9007199254740992.0);
}

double Prng::NextGaussian(void)
{
    double result;

    if (mHasSpare)
    {
        mHasSpare = false;
        result    = mSpareGauss;
    }
    else
    {
        double u1 = NextUnitDouble();
        double u2;
        double radius;
        double theta;

        while (u1 <= std::numeric_limits<double>::min())
        {
            u1 = NextUnitDouble();
        }

        u2          = NextUnitDouble();
        radius      = std::sqrt(-2.0 * std::log(u1));
        theta       = kTwoPi * u2;
        mSpareGauss = radius * std::sin(theta);
        mHasSpare   = true;
        result      = radius * std::cos(theta);
    }

    return result;
}

double Prng::NextExponential(void)
{
    double u = NextUnitDouble();

    while (u <= std::numeric_limits<double>::min())
    {
        u = NextUnitDouble();
    }

    return -std::log(u);
}

} // namespace Rf
} // namespace otns
