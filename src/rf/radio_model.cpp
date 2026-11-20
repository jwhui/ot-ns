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

#include "rf/radio_model.hpp"

#include <algorithm>
#include <cmath>

#include "rf/ber_model.hpp"

namespace otns {
namespace Rf {

RadioModel::RadioModel(RadioModelPreset aPreset, uint64_t aSeed)
    : mParams(RadioModelParams::FromPreset(aPreset))
    , mRng(aSeed)
    , mFading(aSeed)
    , mSpatial(kDefaultRadioRange)
{
}

void RadioModel::SetPreset(RadioModelPreset aPreset) { SetParams(RadioModelParams::FromPreset(aPreset)); }

void RadioModel::SetParams(const RadioModelParams &aParams)
{
    bool invalidateFading = (mParams.mShadowFadingSigmaDb != aParams.mShadowFadingSigmaDb) ||
                            (mParams.mTimeFadingSigmaMaxDb != aParams.mTimeFadingSigmaMaxDb) ||
                            (mParams.mMeterPerUnit != aParams.mMeterPerUnit) ||
                            (mParams.mMeanTimeFadingChange != aParams.mMeanTimeFadingChange);

    mParams = aParams;

    if (invalidateFading)
    {
        mFading.ClearCache();
    }
}

void RadioModel::ResetSeed(uint64_t aSeed)
{
    mRng.Seed(aSeed);
    mFading.Reset(aSeed);
}

void RadioModel::Clear(uint64_t aSeed)
{
    mNodes.clear();
    mSpatial.Clear();
    mActiveTx.clear();
    mActiveTxByCh.clear();
    mActiveScanByCh.clear();
    mNowUs = 0;
    ResetSeed(aSeed);
}

void RadioModel::AdvanceTime(uint64_t aNowUs)
{
    mNowUs = aNowUs;
    mFading.AdvanceTime(aNowUs);
}

void RadioModel::AddNode(const RadioNodeState &aNode)
{
    mNodes[aNode.mId] = aNode;
    mSpatial.Upsert(aNode.mId, aNode.mPos);
}

void RadioModel::RemoveNode(NodeId aId)
{
    mNodes.erase(aId);
    mSpatial.Remove(aId);
    mActiveTx.erase(aId);

    for (auto &pair : mActiveTxByCh)
    {
        pair.second.erase(aId);
    }

    for (auto &pair : mActiveScanByCh)
    {
        pair.second.erase(aId);
    }

    for (auto &pair : mActiveTx)
    {
        pair.second.mInterferers.erase(aId);
    }
}

const RadioNodeState *RadioModel::GetNode(NodeId aId) const
{
    auto it = mNodes.find(aId);

    return (it != mNodes.end()) ? &it->second : nullptr;
}

void RadioModel::SetNodePosition(NodeId aId, const Position3D &aPos)
{
    auto it = mNodes.find(aId);

    if (it != mNodes.end())
    {
        it->second.mPos = aPos;
        mSpatial.Upsert(aId, aPos);
    }
}

void RadioModel::SetNodeRadioState(NodeId aId, RadioState aState, ChannelId aChannel)
{
    auto it = mNodes.find(aId);

    if (it != mNodes.end())
    {
        it->second.mState   = aState;
        it->second.mChannel = aChannel;
    }
}

void RadioModel::SetNodeTxPower(NodeId aId, DbValue aTxPowerDbm)
{
    auto it = mNodes.find(aId);

    if (it != mNodes.end())
    {
        it->second.mTxPowerDbm = aTxPowerDbm;
    }
}

void RadioModel::SetNodeRxSensitivity(NodeId aId, DbValue aRxSensitivityDbm)
{
    auto it = mNodes.find(aId);

    if (it != mNodes.end())
    {
        it->second.mRxSensitivityDbm = aRxSensitivityDbm;
    }
}

void RadioModel::SetNodeRadioRange(NodeId aId, double aRadioRange)
{
    auto it = mNodes.find(aId);

    if (it != mNodes.end())
    {
        it->second.mRadioRange = aRadioRange;
    }
}

DbValue RadioModel::ComputeTxRssi(const RadioNodeState &aSrc, const RadioNodeState &aDst)
{
    double  dist = aSrc.mPos.DistanceTo(aDst.mPos);
    DbValue rssi = mParams.mRssiMaxDbm;

    if (mParams.mIsDiscLimit && dist > aSrc.mRadioRange)
    {
        return kRssiMinusInfinityDbm;
    }

    if (mParams.mRssiMinDbm < mParams.mRssiMaxDbm)
    {
        if (mParams.mUse3gppPathloss)
        {
            rssi = ComputeIndoorRssi3gpp(dist, aSrc.mTxPowerDbm, mParams);
        }
        else
        {
            rssi = ComputeIndoorRssiItu(dist, aSrc.mTxPowerDbm, mParams);
        }

        if (mParams.mShadowFadingSigmaDb > 0.0 || mParams.mTimeFadingSigmaMaxDb > 0.0)
        {
            rssi -= mFading.ComputeFadingDb(aSrc.mPos, aDst.mPos, mParams);
        }

        rssi = std::clamp(rssi, mParams.mRssiMinDbm, mParams.mRssiMaxDbm);
    }

    return rssi;
}

bool RadioModel::IsReachableNodes(const RadioNodeState &aSrc, const RadioNodeState &aDst)
{
    if (aSrc.mId == aDst.mId || aDst.mState != RadioState::kRx || aSrc.mChannel != aDst.mChannel)
    {
        return false;
    }

    if (mParams.mIsDiscLimit)
    {
        if (aSrc.mPos.DistanceTo(aDst.mPos) > aSrc.mRadioRange)
        {
            return false;
        }

        if (!mParams.mEnableInterference)
        {
            return true;
        }
    }

    DbValue rssi     = ComputeTxRssi(aSrc, aDst);
    DbValue floorDbm = std::max(aDst.mRxSensitivityDbm, mParams.mNoiseFloorDbm) + mParams.mSnrMinThresholdDb;

    return rssi >= kRssiMinDbm && rssi <= kRssiMaxDbm && rssi >= floorDbm;
}

double RadioModel::GetMaxSearchRadius(const RadioNodeState &aSrc) const
{
    if (mParams.mIsDiscLimit)
    {
        return aSrc.mRadioRange;
    }

    if (mParams.mShadowFadingSigmaDb <= 0.0 && mParams.mTimeFadingSigmaMaxDb <= 0.0 && mParams.mExponentDb > 0.0 &&
        mParams.mMeterPerUnit > 0.0)
    {
        DbValue maxPathloss = aSrc.mTxPowerDbm - (kRssiMinDbm + mParams.mSnrMinThresholdDb);
        double  expMeters   = (maxPathloss - mParams.mFixedLossDb) / mParams.mExponentDb;

        if (expMeters > 0.0 && expMeters < 6.0)
        {
            return std::pow(10.0, expMeters) / mParams.mMeterPerUnit;
        }
    }

    return -1.0;
}

DbValue RadioModel::GetTxRssi(NodeId aSrcId, NodeId aDstId)
{
    auto sit = mNodes.find(aSrcId);
    auto dit = mNodes.find(aDstId);

    if (sit == mNodes.end() || dit == mNodes.end())
    {
        return kRssiMinusInfinityDbm;
    }

    return ComputeTxRssi(sit->second, dit->second);
}

bool RadioModel::IsReachable(NodeId aSrcId, NodeId aDstId)
{
    auto sit = mNodes.find(aSrcId);
    auto dit = mNodes.find(aDstId);

    if (sit == mNodes.end() || dit == mNodes.end())
    {
        return false;
    }

    return IsReachableNodes(sit->second, dit->second);
}

DbValue RadioModel::SampleChannelRssi(NodeId aNodeId, ChannelId aChannel)
{
    if (!mParams.mEnableInterference)
    {
        return kRssiMinusInfinityDbm;
    }

    auto nit = mNodes.find(aNodeId);

    if (nit == mNodes.end())
    {
        return kRssiMinusInfinityDbm;
    }

    DbValue totalDbm = mParams.mNoiseFloorDbm;
    auto    tit      = mActiveTxByCh.find(aChannel);

    if (tit != mActiveTxByCh.end())
    {
        for (NodeId txId : tit->second)
        {
            if (txId == aNodeId)
            {
                continue;
            }

            auto srcIt = mNodes.find(txId);

            if (srcIt == mNodes.end())
            {
                continue;
            }

            DbValue rssi = ComputeTxRssi(srcIt->second, nit->second);

            if (rssi > kRssiMinusInfinityDbm)
            {
                totalDbm = AddSignalPowersDbm(rssi, totalDbm);
            }
        }
    }

    return totalDbm;
}

TxStartResult RadioModel::BeginTx(NodeId    aSrcId,
                                  ChannelId aChannel,
                                  DbValue   aTxPowerDbm,
                                  size_t    aPsduLenBytes,
                                  uint64_t  aDurationUs)
{
    TxStartResult result;
    auto          sit = mNodes.find(aSrcId);

    if (sit == mNodes.end())
    {
        result.mAborted = true;
        return result;
    }

    RadioNodeState &src = sit->second;

    if (mParams.mEnableInterference)
    {
        bool alreadyTx   = mActiveTxByCh[aChannel].count(aSrcId) > 0;
        bool alreadyScan = mActiveScanByCh[aChannel].count(aSrcId) > 0;

        if (alreadyTx || alreadyScan)
        {
            result.mAborted = true;
            return result;
        }
    }

    src.mTxPowerDbm = aTxPowerDbm;
    src.mChannel    = aChannel;
    src.mState      = RadioState::kTx;
    src.mStats.mTxFrames += 1;
    src.mStats.mTxBytes += aPsduLenBytes + kPhyHeaderLenBytes;
    src.mStats.mTxTimeUs += aDurationUs;

    ActiveTxInfo txInfo;

    txInfo.mChannel    = aChannel;
    txInfo.mTxPowerDbm = aTxPowerDbm;

    if (mParams.mEnableInterference)
    {
        auto &chTx = mActiveTxByCh[aChannel];

        for (NodeId otherTxId : chTx)
        {
            txInfo.mInterferers.insert(otherTxId);
            mActiveTx[otherTxId].mInterferers.insert(aSrcId);
        }

        chTx.insert(aSrcId);

        for (NodeId scannerId : mActiveScanByCh[aChannel])
        {
            auto scanIt = mNodes.find(scannerId);

            if (scanIt == mNodes.end() || scannerId == aSrcId)
            {
                continue;
            }

            DbValue rssi = ComputeTxRssi(src, scanIt->second);

            if (rssi > kRssiMinusInfinityDbm)
            {
                scanIt->second.mRssiSampleMaxDbm = AddSignalPowersDbm(rssi, scanIt->second.mRssiSampleMaxDbm);
            }
        }
    }

    mActiveTx[aSrcId] = std::move(txInfo);

    std::vector<NodeId> candidates = mSpatial.QueryRadius(src.mPos, GetMaxSearchRadius(src));

    for (NodeId dstId : candidates)
    {
        if (dstId == aSrcId)
        {
            continue;
        }

        auto dit = mNodes.find(dstId);

        if (dit == mNodes.end() || !IsReachableNodes(src, dit->second))
        {
            continue;
        }

        DbValue rssiRaw = ComputeTxRssi(src, dit->second);

        result.mReceivers.push_back({dstId, ClipRssiToInt8(rssiRaw), rssiRaw});
    }

    return result;
}

TxEndResult RadioModel::EndTx(NodeId aSrcId, ChannelId aChannel, size_t aPsduLenBytes)
{
    TxEndResult result;
    auto        sit = mNodes.find(aSrcId);
    auto        ait = mActiveTx.find(aSrcId);

    if (sit == mNodes.end() || ait == mActiveTx.end())
    {
        result.mAborted = true;
        return result;
    }

    RadioNodeState &src       = sit->second;
    ActiveTxInfo    txInfo    = std::move(ait->second);
    bool            chChanged = (aChannel != src.mChannel);

    mActiveTx.erase(ait);

    if (mParams.mEnableInterference)
    {
        mActiveTxByCh[txInfo.mChannel].erase(aSrcId);
        mActiveTxByCh[src.mChannel].erase(aSrcId);
    }

    std::vector<NodeId> candidates = mSpatial.QueryRadius(src.mPos, GetMaxSearchRadius(src));

    for (NodeId dstId : candidates)
    {
        if (dstId == aSrcId)
        {
            continue;
        }

        auto dit = mNodes.find(dstId);

        if (dit == mNodes.end())
        {
            continue;
        }

        RadioNodeState &dst = dit->second;

        if (!IsReachableNodes(src, dst))
        {
            continue;
        }

        DbValue   rssi = ComputeTxRssi(src, dst);
        RxOutcome outcome;

        outcome.mNodeId  = dstId;
        outcome.mRssiDbm = ClipRssiToInt8(rssi);

        if (chChanged)
        {
            outcome.mStatus = RxResultStatus::kFcsError;
            dst.mStats.mRxFramesFcsErr += 1;
            result.mDeliveries.push_back(outcome);
            continue;
        }

        if (!mParams.mEnableInterference)
        {
            outcome.mSirDb  = rssi - kRssiMinusInfinityDbm;
            outcome.mStatus = RxResultStatus::kOk;
            dst.mStats.mRxFramesOk += 1;
            result.mDeliveries.push_back(outcome);
            continue;
        }

        DbValue powIntfMax = mParams.mNoiseFloorDbm;
        bool    selfTx     = false;

        for (NodeId intfId : txInfo.mInterferers)
        {
            if (intfId == dstId)
            {
                selfTx = true;
                break;
            }

            auto iit = mNodes.find(intfId);

            if (iit == mNodes.end())
            {
                continue;
            }

            DbValue powIntf = ComputeTxRssi(iit->second, dst);

            powIntfMax = AddSignalPowersDbm(powIntf, powIntfMax);
        }

        if (selfTx)
        {
            outcome.mStatus = RxResultStatus::kAbort;
            result.mDeliveries.push_back(outcome);
            continue;
        }

        outcome.mSirDb = rssi - powIntfMax;
        if (EvaluateFrameSuccess(outcome.mSirDb, aPsduLenBytes, mRng))
        {
            outcome.mStatus = RxResultStatus::kOk;
            dst.mStats.mRxFramesOk += 1;
        }
        else
        {
            outcome.mStatus = RxResultStatus::kFcsError;
            dst.mStats.mRxFramesFcsErr += 1;
        }

        result.mDeliveries.push_back(outcome);
    }

    return result;
}

bool RadioModel::BeginEnergyScan(NodeId aNodeId, ChannelId aChannel)
{
    auto nit = mNodes.find(aNodeId);

    if (nit == mNodes.end())
    {
        return false;
    }

    RadioNodeState &node = nit->second;

    node.mStats.mChanSampleCnt += 1;

    if (!mParams.mEnableInterference)
    {
        node.mChannel          = aChannel;
        node.mRssiSampleMaxDbm = kRssiMinusInfinityDbm;
        return true;
    }

    if (mActiveTxByCh[aChannel].count(aNodeId) > 0 || mActiveScanByCh[aChannel].count(aNodeId) > 0)
    {
        return false;
    }

    node.mChannel          = aChannel;
    node.mRssiSampleMaxDbm = SampleChannelRssi(aNodeId, aChannel);
    mActiveScanByCh[aChannel].insert(aNodeId);

    return true;
}

int8_t RadioModel::EndEnergyScan(NodeId aNodeId, ChannelId aChannel)
{
    auto nit = mNodes.find(aNodeId);

    if (nit == mNodes.end())
    {
        return kRssiInvalidInt8;
    }

    RadioNodeState &node = nit->second;

    if (!mParams.mEnableInterference)
    {
        return ClipRssiToInt8(kRssiMinusInfinityDbm);
    }

    mActiveScanByCh[aChannel].erase(aNodeId);

    DbValue finalSample = SampleChannelRssi(aNodeId, aChannel);

    if (finalSample > node.mRssiSampleMaxDbm)
    {
        node.mRssiSampleMaxDbm = finalSample;
    }

    return ClipRssiToInt8(node.mRssiSampleMaxDbm);
}

} // namespace Rf
} // namespace otns
