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

#include "core/kpi_tracker.hpp"

#include <cstdio>

#include "core/path_security.hpp"

namespace otns {
namespace Core {

namespace {

nlohmann::json NodeKpiToJson(const NodeKpi &aKpi)
{
    nlohmann::json obj = nlohmann::json::object();

    obj["tx_frames"]         = aKpi.mTxFrames;
    obj["tx_bytes"]          = aKpi.mTxBytes;
    obj["tx_airtime_us"]     = aKpi.mTxAirtimeUs;
    obj["tx_acked"]          = aKpi.mTxAcked;
    obj["tx_no_ack"]         = aKpi.mTxNoAck;
    obj["tx_cca_fail"]       = aKpi.mTxCcaFail;
    obj["rx_ok"]             = aKpi.mRxOk;
    obj["rx_fcs_err"]        = aKpi.mRxFcsErr;
    obj["rx_bytes"]          = aKpi.mRxBytes;
    obj["mac_data_frames"]   = aKpi.mMacDataFrames;
    obj["mac_ack_frames"]    = aKpi.mMacAckFrames;
    obj["mac_cmd_frames"]    = aKpi.mMacCmdFrames;
    obj["mac_beacon_frames"] = aKpi.mMacBeaconFrames;
    obj["role_changes"]      = aKpi.mRoleChanges;

    return obj;
}

void AccumulateKpi(NodeKpi &aDst, const NodeKpi &aSrc)
{
    aDst.mTxFrames += aSrc.mTxFrames;
    aDst.mTxBytes += aSrc.mTxBytes;
    aDst.mTxAirtimeUs += aSrc.mTxAirtimeUs;
    aDst.mTxAcked += aSrc.mTxAcked;
    aDst.mTxNoAck += aSrc.mTxNoAck;
    aDst.mTxCcaFail += aSrc.mTxCcaFail;
    aDst.mRxOk += aSrc.mRxOk;
    aDst.mRxFcsErr += aSrc.mRxFcsErr;
    aDst.mRxBytes += aSrc.mRxBytes;
    aDst.mMacDataFrames += aSrc.mMacDataFrames;
    aDst.mMacAckFrames += aSrc.mMacAckFrames;
    aDst.mMacCmdFrames += aSrc.mMacCmdFrames;
    aDst.mMacBeaconFrames += aSrc.mMacBeaconFrames;
    aDst.mRoleChanges += aSrc.mRoleChanges;
}

} // namespace

void KpiTracker::Reset(void)
{
    mNodes.clear();
    mLastRole.clear();
}

void KpiTracker::RecordTxStart(Rf::NodeId aNodeId, size_t aPsduBytes, uint64_t aDurationUs, const MacFrameInfo &aMac)
{
    NodeKpi &kpi = mNodes[aNodeId];

    kpi.mTxFrames += 1;
    kpi.mTxBytes += aPsduBytes;
    kpi.mTxAirtimeUs += aDurationUs;

    if (aMac.mValid)
    {
        switch (aMac.mFrameType)
        {
        case MacFrameType::kBeacon:
            kpi.mMacBeaconFrames += 1;
            break;
        case MacFrameType::kData:
            kpi.mMacDataFrames += 1;
            break;
        case MacFrameType::kAck:
            kpi.mMacAckFrames += 1;
            break;
        case MacFrameType::kCommand:
            kpi.mMacCmdFrames += 1;
            break;
        default:
            break;
        }
    }
}

void KpiTracker::RecordTxOutcome(Rf::NodeId aNodeId, bool aAckRequested, bool aAcked, bool aCcaFail)
{
    NodeKpi &kpi = mNodes[aNodeId];

    if (aCcaFail)
    {
        kpi.mTxCcaFail += 1;
        return;
    }

    if (aAckRequested)
    {
        if (aAcked)
        {
            kpi.mTxAcked += 1;
        }
        else
        {
            kpi.mTxNoAck += 1;
        }
    }
}

void KpiTracker::RecordRxOutcome(Rf::NodeId aNodeId, size_t aPsduBytes, bool aOk)
{
    NodeKpi &kpi = mNodes[aNodeId];

    if (aOk)
    {
        kpi.mRxOk += 1;
        kpi.mRxBytes += aPsduBytes;
    }
    else
    {
        kpi.mRxFcsErr += 1;
    }
}

void KpiTracker::RecordRoleObservation(Rf::NodeId aNodeId, DeviceRole aRole)
{
    auto it = mLastRole.find(aNodeId);

    if (it == mLastRole.end())
    {
        mLastRole[aNodeId] = aRole;
    }
    else if (it->second != aRole)
    {
        it->second = aRole;
        mNodes[aNodeId].mRoleChanges += 1;
    }
}

KpiSummary KpiTracker::GetSummary(uint64_t aNowUs) const
{
    KpiSummary summary;

    summary.mSimDurationUs = aNowUs;
    summary.mNodes         = mNodes;

    for (const auto &pair : mNodes)
    {
        AccumulateKpi(summary.mTotal, pair.second);
    }

    return summary;
}

nlohmann::json KpiTracker::ToJson(uint64_t aNowUs) const
{
    KpiSummary     summary  = GetSummary(aNowUs);
    nlohmann::json root     = nlohmann::json::object();
    nlohmann::json nodesObj = nlohmann::json::object();

    root["version"]         = 1;
    root["sim_duration_us"] = summary.mSimDurationUs;
    root["total"]           = NodeKpiToJson(summary.mTotal);

    if (summary.mSimDurationUs > 0)
    {
        root["channel_utilization_ratio"] =
            static_cast<double>(summary.mTotal.mTxAirtimeUs) / static_cast<double>(summary.mSimDurationUs);
    }
    else
    {
        root["channel_utilization_ratio"] = 0.0;
    }

    for (const auto &pair : summary.mNodes)
    {
        nodesObj[std::to_string(pair.first)] = NodeKpiToJson(pair.second);
    }
    root["nodes"] = std::move(nodesObj);

    return root;
}

bool KpiTracker::SaveJsonContained(const std::filesystem::path &aBaseDir,
                                   const std::string           &aRequestedPath,
                                   uint64_t                     aNowUs) const
{
    std::filesystem::path resolved;

    if (!ResolveContainedPath(aBaseDir, aRequestedPath, resolved))
    {
        return false;
    }

    std::error_code       ec;
    std::filesystem::path parent = resolved.parent_path();

    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            return false;
        }
    }

    std::string payload = ToJson(aNowUs).dump(2) + "\n";
    std::FILE  *fp      = std::fopen(resolved.c_str(), "wb");

    if (fp == nullptr)
    {
        return false;
    }

    bool ok = (std::fwrite(payload.data(), 1, payload.size(), fp) == payload.size()) && (std::fflush(fp) == 0);

    std::fclose(fp);

    return ok;
}

} // namespace Core
} // namespace otns
