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

#ifndef OTNS_RF_RADIO_MODEL_HPP_
#define OTNS_RF_RADIO_MODEL_HPP_

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "rf/pathloss_fading.hpp"
#include "rf/prng.hpp"
#include "rf/spatial_index.hpp"
#include "rf/types.hpp"

namespace otns {
namespace Rf {

struct RxCandidate
{
    NodeId  mNodeId  = kInvalidNodeId;
    int8_t  mRssiDbm = kRssiInvalidInt8;
    DbValue mRssiRaw = kRssiMinusInfinityDbm;
};

struct RxOutcome
{
    NodeId         mNodeId  = kInvalidNodeId;
    int8_t         mRssiDbm = kRssiInvalidInt8;
    DbValue        mSirDb   = 0.0;
    RxResultStatus mStatus  = RxResultStatus::kOk;
};

struct TxStartResult
{
    bool                     mAborted = false;
    std::vector<RxCandidate> mReceivers;
};

struct TxEndResult
{
    bool                   mAborted = false;
    std::vector<RxOutcome> mDeliveries;
};

/**
 * Standalone, deterministic RF propagation and interference engine (zero OS/OpenThread deps).
 *
 * Uses std::map and sorted NodeId vectors throughout for deterministic cross-platform behavior.
 */
class RadioModel
{
public:
    explicit RadioModel(RadioModelPreset aPreset = RadioModelPreset::kMutualInterference, uint64_t aSeed = 1);

    void                    SetPreset(RadioModelPreset aPreset);
    void                    SetParams(const RadioModelParams &aParams);
    const RadioModelParams &GetParams(void) const { return mParams; }

    void ResetSeed(uint64_t aSeed);
    void Clear(uint64_t aSeed);
    void AdvanceTime(uint64_t aNowUs);

    void                  AddNode(const RadioNodeState &aNode);
    void                  RemoveNode(NodeId aId);
    bool                  HasNode(NodeId aId) const { return mNodes.find(aId) != mNodes.end(); }
    const RadioNodeState *GetNode(NodeId aId) const;

    void SetNodePosition(NodeId aId, const Position3D &aPos);
    void SetNodeRadioState(NodeId aId, RadioState aState, ChannelId aChannel);
    void SetNodeTxPower(NodeId aId, DbValue aTxPowerDbm);
    void SetNodeRxSensitivity(NodeId aId, DbValue aRxSensitivityDbm);
    void SetNodeRadioRange(NodeId aId, double aRadioRange);

    DbValue GetTxRssi(NodeId aSrcId, NodeId aDstId);
    bool    IsReachable(NodeId aSrcId, NodeId aDstId);
    DbValue SampleChannelRssi(NodeId aNodeId, ChannelId aChannel);

    TxStartResult BeginTx(NodeId    aSrcId,
                          ChannelId aChannel,
                          DbValue   aTxPowerDbm,
                          size_t    aPsduLenBytes,
                          uint64_t  aDurationUs);
    TxEndResult   EndTx(NodeId aSrcId, ChannelId aChannel, size_t aPsduLenBytes);

    bool   BeginEnergyScan(NodeId aNodeId, ChannelId aChannel);
    int8_t EndEnergyScan(NodeId aNodeId, ChannelId aChannel);

    size_t GetNodeCount(void) const { return mNodes.size(); }

private:
    struct ActiveTxInfo
    {
        ChannelId        mChannel    = kMinChannel;
        DbValue          mTxPowerDbm = kDefaultTxPowerDbm;
        std::set<NodeId> mInterferers; // Sorted for deterministic power summation
    };

    DbValue ComputeTxRssi(const RadioNodeState &aSrc, const RadioNodeState &aDst);
    bool    IsReachableNodes(const RadioNodeState &aSrc, const RadioNodeState &aDst);
    double  GetMaxSearchRadius(const RadioNodeState &aSrc) const;

    RadioModelParams                      mParams;
    Prng                                  mRng;
    FadingModel                           mFading;
    SpatialIndex                          mSpatial;
    uint64_t                              mNowUs = 0;
    std::map<NodeId, RadioNodeState>      mNodes;
    std::map<ChannelId, std::set<NodeId>> mActiveTxByCh;
    std::map<ChannelId, std::set<NodeId>> mActiveScanByCh;
    std::map<NodeId, ActiveTxInfo>        mActiveTx;
};

} // namespace Rf
} // namespace otns

#endif // OTNS_RF_RADIO_MODEL_HPP_
