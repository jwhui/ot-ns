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

#include "rf/ber_model.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace otns {
namespace Rf {

namespace {

static constexpr std::array<double, 15> kBinomialCoeffs = {
    120.0,  -560.0,  1820.0, -4368.0, 8008.0, -11440.0, 12870.0, -11440.0,
    8008.0, -4368.0, 1820.0, -560.0,  120.0,  -16.0,    1.0,
};

} // namespace

double ComputePacketSuccessRate(DbValue aSirDb, size_t aPsduLenBytes)
{
    double result;

    if (aSirDb >= 6.0)
    {
        result = 1.0;
    }
    else if (aSirDb < -10.0)
    {
        result = 0.0;
    }
    else
    {
        size_t berBits = (kPhySfdLenBytes + kPhyPhrLenBytes + aPsduLenBytes) * 8u;
        double snr     = std::pow(10.0, aSirDb / 10.0);
        double ber     = 0.0;

        for (size_t idx = 0; idx < kBinomialCoeffs.size(); ++idx)
        {
            double k = static_cast<double>(idx + 2u);

            ber += kBinomialCoeffs[idx] * std::exp(20.0 * snr * ((1.0 / k) - 1.0));
        }

        ber    = std::clamp(ber * (8.0 / 15.0) / 16.0, 0.0, 1.0);
        result = std::pow(1.0 - ber, static_cast<double>(berBits));
    }

    return result;
}

bool EvaluateFrameSuccess(DbValue aSirDb, size_t aPsduLenBytes, Prng &aRng)
{
    bool success;

    if (aSirDb >= 6.0)
    {
        success = true;
    }
    else if (aSirDb < -10.0)
    {
        success = false;
    }
    else
    {
        double pSuccess = ComputePacketSuccessRate(aSirDb, aPsduLenBytes);

        success = (pSuccess >= 1.0) || (aRng.NextUnitDouble() <= pSuccess);
    }

    return success;
}

} // namespace Rf
} // namespace otns
