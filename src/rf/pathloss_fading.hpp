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

#ifndef OTNS_RF_PATHLOSS_FADING_HPP_
#define OTNS_RF_PATHLOSS_FADING_HPP_

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "rf/prng.hpp"
#include "rf/types.hpp"

namespace otns {
namespace Rf {

double  RoundParam2(double aValue);
DbValue AddSignalPowersDbm(DbValue aPower1, DbValue aPower2);
int8_t  ClipRssiToInt8(DbValue aRssi);

DbValue ComputeIndoorRssiItu(double aDistUnits, DbValue aTxPowerDbm, const RadioModelParams &aParams);
DbValue ComputeIndoorRssi3gpp(double aDistUnits, DbValue aTxPowerDbm, const RadioModelParams &aParams);

/**
 * Symmetric position-dependent Shadow Fading (SF) and Time-Variant Fading (TVF).
 */
class FadingModel
{
public:
    explicit FadingModel(uint64_t aSeed = 1);

    void    Reset(uint64_t aSeed);
    void    ClearCache(void);
    void    AdvanceTime(uint64_t aNowUs);
    DbValue ComputeFadingDb(const Position3D &aSrc, const Position3D &aDst, const RadioModelParams &aParams);
    size_t  GetCacheSize(void) const { return mCache.size(); }

private:
    struct LinkFadeState
    {
        Prng     mRng{1};
        DbValue  mSfDb           = 0.0;
        DbValue  mTvfSigmaDb     = 0.0;
        DbValue  mTvfDb          = 0.0;
        uint64_t mNextTvfChangeU = 0;
    };

    static uint64_t CalcLinkUid(const Position3D &aSrc, const Position3D &aDst, double aMeterPerUnit);

    uint64_t                                    mSeed  = 1;
    uint64_t                                    mNowUs = 0;
    std::unordered_map<uint64_t, LinkFadeState> mCache;
};

} // namespace Rf
} // namespace otns

#endif // OTNS_RF_PATHLOSS_FADING_HPP_
