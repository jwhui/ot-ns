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

#ifndef OTNS_RF_TYPES_HPP_
#define OTNS_RF_TYPES_HPP_

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace otns {
namespace Rf {

using NodeId    = uint32_t;
using ChannelId = uint8_t;
using DbValue   = double;

static constexpr NodeId    kInvalidNodeId        = 0;
static constexpr ChannelId kMinChannel           = 11;
static constexpr ChannelId kMaxChannel           = 26;
static constexpr DbValue   kRssiMinDbm           = -127.0;
static constexpr DbValue   kRssiMaxDbm           = 127.0;
static constexpr DbValue   kRssiMinusInfinityDbm = -127.0;
static constexpr int8_t    kRssiInvalidInt8      = 127;
static constexpr DbValue   kDefaultNoiseFloorDbm = -95.0;
static constexpr DbValue   kDefaultRxSensitivity = -100.0;
static constexpr DbValue   kDefaultTxPowerDbm    = 0.0;
static constexpr double    kDefaultMeterPerUnit  = 0.10;
static constexpr double    kDefaultRadioRange    = 160.0; // units (16 m at 0.10 m/unit)
static constexpr size_t    kPhyPreambleLenBytes  = 4;
static constexpr size_t    kPhySfdLenBytes       = 1;
static constexpr size_t    kPhyPhrLenBytes       = 1;
static constexpr size_t    kPhyHeaderLenBytes    = kPhyPreambleLenBytes + kPhySfdLenBytes + kPhyPhrLenBytes;
static constexpr double    kUndefinedParam       = std::numeric_limits<double>::quiet_NaN();

enum class RadioState : uint8_t
{
    kDisabled = 0,
    kSleep    = 1,
    kRx       = 2,
    kTx       = 3,
};

enum class RadioModelPreset : uint8_t
{
    kIdeal,              // Disc range limit, constant ideal RSSI, infinite parallel TX
    kIdealRssi,          // Disc range limit, ITU indoor pathloss RSSI, infinite parallel TX
    kMutualInterference, // 3GPP indoor pathloss + shadow/time fading + SINR/BER + CCA
    kMidisc,             // MutualInterference with hard disc range limit
    kOutdoor,            // Outdoor LoS pathloss + lighter fading + SINR/BER + CCA
};

enum class RxResultStatus : uint8_t
{
    kOk       = 0,
    kFcsError = 1, // Corrupted by interference/noise via BER model
    kAbort    = 2, // Aborted (e.g., receiver was transmitting or channel changed)
};

struct Position3D
{
    double DistanceSquaredTo(const Position3D &aOther) const
    {
        double dx = mX - aOther.mX;
        double dy = mY - aOther.mY;
        double dz = mZ - aOther.mZ;

        return dx * dx + dy * dy + dz * dz;
    }

    double DistanceTo(const Position3D &aOther) const { return std::sqrt(DistanceSquaredTo(aOther)); }

    double mX = 0.0;
    double mY = 0.0;
    double mZ = 0.0;
};

struct RadioModelParams
{
    static RadioModelParams FromPreset(RadioModelPreset aPreset);

    double  mMeterPerUnit         = kDefaultMeterPerUnit;
    bool    mIsDiscLimit          = false;
    bool    mUse3gppPathloss      = true;
    bool    mEnableInterference   = true;
    DbValue mRssiMinDbm           = kRssiMinDbm;
    DbValue mRssiMaxDbm           = kRssiMaxDbm;
    DbValue mExponentDb           = 17.3;
    DbValue mFixedLossDb          = 40.0;
    DbValue mNlosExponentDb       = 38.3;
    DbValue mNlosFixedLossDb      = 26.77;
    DbValue mNoiseFloorDbm        = kDefaultNoiseFloorDbm;
    DbValue mSnrMinThresholdDb    = -4.0;
    DbValue mShadowFadingSigmaDb  = 8.03;
    DbValue mTimeFadingSigmaMaxDb = 4.0;
    double  mMeanTimeFadingChange = 150.0; // seconds, 0 = disabled
};

struct PhyStats
{
    uint64_t mTxFrames       = 0;
    uint64_t mTxBytes        = 0;
    uint64_t mTxTimeUs       = 0;
    uint64_t mRxFramesOk     = 0;
    uint64_t mRxFramesFcsErr = 0;
    uint64_t mChanSampleCnt  = 0;
};

struct RadioNodeState
{
    NodeId     mId               = kInvalidNodeId;
    Position3D mPos              = {};
    double     mRadioRange       = kDefaultRadioRange;
    ChannelId  mChannel          = kMinChannel;
    DbValue    mTxPowerDbm       = kDefaultTxPowerDbm;
    DbValue    mRxSensitivityDbm = kDefaultRxSensitivity;
    RadioState mState            = RadioState::kDisabled;
    DbValue    mRssiSampleMaxDbm = kRssiMinusInfinityDbm;
    PhyStats   mStats            = {};
};

} // namespace Rf
} // namespace otns

#endif // OTNS_RF_TYPES_HPP_
