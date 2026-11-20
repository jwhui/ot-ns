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

#include "rf/pathloss_fading.hpp"

#include <algorithm>
#include <cmath>

namespace otns {
namespace Rf {

namespace {

static constexpr size_t kMaxFadeCacheEntries = 500000;

} // namespace

double RoundParam2(double aValue) { return std::round(aValue * 100.0) / 100.0; }

DbValue AddSignalPowersDbm(DbValue aPower1, DbValue aPower2)
{
    DbValue result;

    if (aPower1 > aPower2 + 15.0)
    {
        result = aPower1;
    }
    else if (aPower2 > aPower1 + 15.0)
    {
        result = aPower2;
    }
    else
    {
        result = 10.0 * std::log10(std::pow(10.0, aPower1 / 10.0) + std::pow(10.0, aPower2 / 10.0));
    }

    return result;
}

int8_t ClipRssiToInt8(DbValue aRssi)
{
    DbValue rssi = aRssi;

    if (rssi > kRssiMaxDbm)
    {
        rssi = kRssiMaxDbm;
    }
    else if (rssi < kRssiMinDbm)
    {
        rssi = kRssiMinusInfinityDbm;
    }

    return static_cast<int8_t>(std::llround(rssi));
}

RadioModelParams RadioModelParams::FromPreset(RadioModelPreset aPreset)
{
    RadioModelParams params;

    switch (aPreset)
    {
    case RadioModelPreset::kIdeal:
        params.mMeterPerUnit         = kDefaultMeterPerUnit;
        params.mIsDiscLimit          = true;
        params.mUse3gppPathloss      = false;
        params.mEnableInterference   = false;
        params.mRssiMinDbm           = -60.0;
        params.mRssiMaxDbm           = -60.0;
        params.mNoiseFloorDbm        = kRssiMinusInfinityDbm;
        params.mShadowFadingSigmaDb  = 0.0;
        params.mTimeFadingSigmaMaxDb = 0.0;
        params.mMeanTimeFadingChange = 0.0;
        break;

    case RadioModelPreset::kIdealRssi:
        params.mMeterPerUnit         = kDefaultMeterPerUnit;
        params.mIsDiscLimit          = true;
        params.mUse3gppPathloss      = false;
        params.mEnableInterference   = false;
        params.mRssiMinDbm           = kRssiMinDbm;
        params.mRssiMaxDbm           = kRssiMaxDbm;
        params.mExponentDb           = 30.0;
        params.mFixedLossDb          = RoundParam2(20.0 * std::log10(2400.0) - 28.0);
        params.mNoiseFloorDbm        = kRssiMinusInfinityDbm;
        params.mShadowFadingSigmaDb  = 0.0;
        params.mTimeFadingSigmaMaxDb = 0.0;
        params.mMeanTimeFadingChange = 0.0;
        break;

    case RadioModelPreset::kMutualInterference:
    case RadioModelPreset::kMidisc:
        params.mMeterPerUnit         = kDefaultMeterPerUnit;
        params.mIsDiscLimit          = (aPreset == RadioModelPreset::kMidisc);
        params.mUse3gppPathloss      = true;
        params.mEnableInterference   = true;
        params.mRssiMinDbm           = kRssiMinDbm;
        params.mRssiMaxDbm           = kRssiMaxDbm;
        params.mExponentDb           = 17.3;
        params.mFixedLossDb          = RoundParam2(32.4 + 20.0 * std::log10(2.4));
        params.mNlosExponentDb       = 38.3;
        params.mNlosFixedLossDb      = RoundParam2(17.3 + 24.9 * std::log10(2.4));
        params.mNoiseFloorDbm        = kDefaultNoiseFloorDbm;
        params.mSnrMinThresholdDb    = -4.0;
        params.mShadowFadingSigmaDb  = 8.03;
        params.mTimeFadingSigmaMaxDb = 4.0;
        params.mMeanTimeFadingChange = 150.0;
        break;

    case RadioModelPreset::kOutdoor:
        params.mMeterPerUnit         = 0.5;
        params.mIsDiscLimit          = false;
        params.mUse3gppPathloss      = true;
        params.mEnableInterference   = true;
        params.mRssiMinDbm           = kRssiMinDbm;
        params.mRssiMaxDbm           = kRssiMaxDbm;
        params.mExponentDb           = 17.3;
        params.mFixedLossDb          = RoundParam2(32.4 + 20.0 * std::log10(2.4));
        params.mNlosExponentDb       = 0.0;
        params.mNlosFixedLossDb      = 0.0;
        params.mNoiseFloorDbm        = kDefaultNoiseFloorDbm;
        params.mSnrMinThresholdDb    = -4.0;
        params.mShadowFadingSigmaDb  = 3.0;
        params.mTimeFadingSigmaMaxDb = 1.0;
        params.mMeanTimeFadingChange = 150.0;
        break;
    }

    return params;
}

DbValue ComputeIndoorRssiItu(double aDistUnits, DbValue aTxPowerDbm, const RadioModelParams &aParams)
{
    double pathloss   = 0.0;
    double distMeters = aDistUnits * aParams.mMeterPerUnit;

    if (distMeters >= 0.01)
    {
        pathloss = aParams.mExponentDb * std::log10(distMeters) + aParams.mFixedLossDb;
        if (pathloss < 0.0)
        {
            pathloss = 0.0;
        }
    }

    return aTxPowerDbm - pathloss;
}

DbValue ComputeIndoorRssi3gpp(double aDistUnits, DbValue aTxPowerDbm, const RadioModelParams &aParams)
{
    double pathloss   = 0.0;
    double distMeters = aDistUnits * aParams.mMeterPerUnit;

    if (distMeters >= 0.01)
    {
        pathloss = aParams.mExponentDb * std::log10(distMeters) + aParams.mFixedLossDb;
        if (pathloss < 0.0)
        {
            pathloss = 0.0;
        }

        if (aParams.mNlosExponentDb > 0.0)
        {
            double pathlossNlos = aParams.mNlosExponentDb * std::log10(distMeters) + aParams.mNlosFixedLossDb;

            pathloss = std::max(pathloss, pathlossNlos);
        }
    }

    return aTxPowerDbm - pathloss;
}

FadingModel::FadingModel(uint64_t aSeed)
    : mSeed(aSeed)
{
}

void FadingModel::Reset(uint64_t aSeed)
{
    mSeed  = aSeed;
    mNowUs = 0;
    mCache.clear();
}

void FadingModel::ClearCache(void) { mCache.clear(); }

void FadingModel::AdvanceTime(uint64_t aNowUs)
{
    if (mCache.size() > kMaxFadeCacheEntries)
    {
        mCache.clear();
    }

    mNowUs = aNowUs;
}

uint64_t FadingModel::CalcLinkUid(const Position3D &aSrc, const Position3D &aDst, double aMeterPerUnit)
{
    auto quantize = [aMeterPerUnit](double aCoord, int32_t aOffset, uint64_t aMask) -> uint64_t {
        long long q = std::llround(aCoord * aMeterPerUnit * 0.2) + aOffset;

        return static_cast<uint64_t>(q) & aMask;
    };

    uint64_t x1       = quantize(aSrc.mX, 2048, 0xFFFu);
    uint64_t y1       = quantize(aSrc.mY, 2048, 0xFFFu);
    uint64_t z1       = quantize(aSrc.mZ, 128, 0xFFu);
    uint64_t x2       = quantize(aDst.mX, 2048, 0xFFFu);
    uint64_t y2       = quantize(aDst.mY, 2048, 0xFFFu);
    uint64_t z2       = quantize(aDst.mZ, 128, 0xFFu);
    bool     srcFirst = (x1 < x2) || (x1 == x2 && y1 < y2) || (x1 == x2 && y1 == y2 && z1 < z2);

    uint64_t xL = srcFirst ? x1 : x2;
    uint64_t yL = srcFirst ? y1 : y2;
    uint64_t zL = srcFirst ? z1 : z2;
    uint64_t xR = srcFirst ? x2 : x1;
    uint64_t yR = srcFirst ? y2 : y1;
    uint64_t zR = srcFirst ? z2 : z1;

    return xL | (yL << 12u) | (xR << 24u) | (yR << 36u) | (zL << 48u) | (zR << 56u);
}

DbValue FadingModel::ComputeFadingDb(const Position3D &aSrc, const Position3D &aDst, const RadioModelParams &aParams)
{
    uint64_t linkKey = mSeed + CalcLinkUid(aSrc, aDst, aParams.mMeterPerUnit);
    auto     it      = mCache.find(linkKey);

    if (it != mCache.end())
    {
        LinkFadeState &state = it->second;

        if (aParams.mMeanTimeFadingChange > 0.0 && mNowUs > state.mNextTvfChangeU)
        {
            double deltaSec;

            state.mTvfDb          = state.mRng.NextGaussian() * state.mTvfSigmaDb;
            deltaSec              = state.mRng.NextExponential() * aParams.mMeanTimeFadingChange;
            state.mNextTvfChangeU = mNowUs + static_cast<uint64_t>(deltaSec * 1e6);
        }

        return state.mSfDb + state.mTvfDb;
    }

    LinkFadeState state;

    state.mRng.Seed(linkKey);
    state.mSfDb       = state.mRng.NextGaussian() * aParams.mShadowFadingSigmaDb;
    state.mTvfSigmaDb = state.mRng.NextUnitDouble() * aParams.mTimeFadingSigmaMaxDb;
    state.mTvfDb      = state.mRng.NextGaussian() * state.mTvfSigmaDb;

    if (aParams.mMeanTimeFadingChange > 0.0)
    {
        double deltaSec       = state.mRng.NextExponential() * aParams.mMeanTimeFadingChange;
        state.mNextTvfChangeU = mNowUs + static_cast<uint64_t>(deltaSec * 1e6);
    }

    DbValue total = state.mSfDb + state.mTvfDb;

    mCache.emplace(linkKey, state);

    return total;
}

} // namespace Rf
} // namespace otns
