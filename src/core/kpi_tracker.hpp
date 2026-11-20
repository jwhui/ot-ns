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

#ifndef OTNS_CORE_KPI_TRACKER_HPP_
#define OTNS_CORE_KPI_TRACKER_HPP_

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "core/sim_node.hpp"
#include "core/wpan_dissector.hpp"
#include "nlohmann/json.hpp"
#include "rf/types.hpp"

namespace otns {
namespace Core {

struct NodeKpi
{
    uint64_t mTxFrames        = 0;
    uint64_t mTxBytes         = 0;
    uint64_t mTxAirtimeUs     = 0;
    uint64_t mTxAcked         = 0;
    uint64_t mTxNoAck         = 0;
    uint64_t mTxCcaFail       = 0;
    uint64_t mRxOk            = 0;
    uint64_t mRxFcsErr        = 0;
    uint64_t mRxBytes         = 0;
    uint64_t mMacDataFrames   = 0;
    uint64_t mMacAckFrames    = 0;
    uint64_t mMacCmdFrames    = 0;
    uint64_t mMacBeaconFrames = 0;
    uint64_t mRoleChanges     = 0;
};

struct KpiSummary
{
    uint64_t                      mSimDurationUs = 0;
    NodeKpi                       mTotal;
    std::map<Rf::NodeId, NodeKpi> mNodes;
};

/**
 * Tracks per-node and global radio/MAC/network KPIs and exports structured JSON reports.
 */
class KpiTracker
{
public:
    void Reset(void);
    void RecordTxStart(Rf::NodeId aNodeId, size_t aPsduBytes, uint64_t aDurationUs, const MacFrameInfo &aMac);
    void RecordTxOutcome(Rf::NodeId aNodeId, bool aAckRequested, bool aAcked, bool aCcaFail);
    void RecordRxOutcome(Rf::NodeId aNodeId, size_t aPsduBytes, bool aOk);
    void RecordRoleObservation(Rf::NodeId aNodeId, DeviceRole aRole);

    KpiSummary     GetSummary(uint64_t aNowUs) const;
    nlohmann::json ToJson(uint64_t aNowUs) const;
    bool           SaveJsonContained(const std::filesystem::path &aBaseDir,
                                     const std::string           &aRequestedPath,
                                     uint64_t                     aNowUs) const;

private:
    std::map<Rf::NodeId, NodeKpi>    mNodes;
    std::map<Rf::NodeId, DeviceRole> mLastRole;
};

} // namespace Core
} // namespace otns

#endif // OTNS_CORE_KPI_TRACKER_HPP_
