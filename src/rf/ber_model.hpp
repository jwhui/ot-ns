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

#ifndef OTNS_RF_BER_MODEL_HPP_
#define OTNS_RF_BER_MODEL_HPP_

#include <cstddef>

#include "rf/prng.hpp"
#include "rf/types.hpp"

namespace otns {
namespace Rf {

/**
 * Reference: IEEE 802.15.4-2006, Annex E.4.1.8 Bit Error Rate (BER) calculations
 * (aligned with OTNS PR #880: excludes 4-byte preamble and decouples bit rate).
 */
double ComputePacketSuccessRate(DbValue aSirDb, size_t aPsduLenBytes);

/**
 * Evaluates whether a frame with `aPsduLenBytes` succeeds at `aSirDb`.
 *
 * @retval true   Frame is received cleanly.
 * @retval false  Frame is corrupted (OT_ERROR_FCS).
 */
bool EvaluateFrameSuccess(DbValue aSirDb, size_t aPsduLenBytes, Prng &aRng);

} // namespace Rf
} // namespace otns

#endif // OTNS_RF_BER_MODEL_HPP_
