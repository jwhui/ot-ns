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

#ifndef OTNS_CORE_SIMULATION_HPP_
#define OTNS_CORE_SIMULATION_HPP_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/kpi_tracker.hpp"
#include "core/net_fabric.hpp"
#include "core/pcap_writer.hpp"
#include "core/scheduler.hpp"
#include "core/sim_node.hpp"
#include "core/topology.hpp"
#include "rf/prng.hpp"
#include "rf/radio_model.hpp"

namespace otns {
namespace Core {

struct SimulationConfig
{
    uint64_t             mSeed       = 1;
    Rf::RadioModelPreset mRfPreset   = Rf::RadioModelPreset::kMutualInterference;
    uint16_t             mPanId      = 0xFACE;
    Rf::ChannelId        mChannel    = Rf::kMinChannel;
    std::string          mNetworkKey = "00112233445566778899aabbccddeeff";
    double               mSpeed      = 0.0; // 0.0 = max speed (unpaced); 1.0 = real-time
};

struct SimulationStats
{
    uint32_t mTotal      = 0;
    uint32_t mLeaders    = 0;
    uint32_t mRouters    = 0;
    uint32_t mChildren   = 0;
    uint32_t mDetached   = 0;
    uint32_t mDisabled   = 0;
    uint32_t mPartitions = 0;
    uint64_t mTxFrames   = 0;
    uint64_t mRxOk       = 0;
    uint64_t mRxFcsErr   = 0;
};

struct PacketObservation
{
    uint64_t      mTimestampUs = 0;
    Rf::NodeId    mSrcId       = Rf::kInvalidNodeId;
    Rf::NodeId    mDstId       = Rf::kInvalidNodeId; // 0 = broadcast / unknown
    Rf::ChannelId mChannel     = Rf::kMinChannel;
    uint64_t      mDurationUs  = 0;
    int8_t        mRssiDbm     = Rf::kRssiInvalidInt8;
    uint16_t      mFrameLen    = 0;
    std::string   mProtocol;
    std::string   mSummary;
};

/**
 * Single-threaded deterministic hybrid Thread network simulator.
 */
class Simulation
{
public:
    using TxDoneCallback         = std::function<void(const std::optional<RawFrame> &aAck, uint8_t aOtError)>;
    using EnergyScanDoneCallback = std::function<void(int8_t aPeakRssiDbm)>;
    using PacketObserver         = std::function<void(const PacketObservation &aPacket)>;

    explicit Simulation(const SimulationConfig &aConfig = {});
    ~Simulation(void);

    Simulation(const Simulation &)            = delete;
    Simulation &operator=(const Simulation &) = delete;

    void Reset(std::optional<uint64_t> aSeed = std::nullopt);
    void SetRfPreset(Rf::RadioModelPreset aPreset);
    void SetPacketObserver(PacketObserver aObserver) { mPacketObserver = std::move(aObserver); }

    Rf::NodeId                  AddNode(const NodeConfig &aConfig = {});
    Rf::NodeId                  RegisterExternalNode(std::unique_ptr<SimNode> aNode, const NodeConfig &aConfig);
    Rf::NodeId                  AllocateNodeId(Rf::NodeId aPreferredId = Rf::kInvalidNodeId);
    bool                        RemoveNode(Rf::NodeId aId);
    bool                        MoveNode(Rf::NodeId aId, Rf::Position3D aPos);
    bool                        SetNodeEnabled(Rf::NodeId aId, bool aEnabled);
    std::string                 ExecuteCli(Rf::NodeId aId, const std::string &aCommand);
    std::string                 TakeCliOutput(Rf::NodeId aId);
    std::optional<NodeSnapshot> GetNodeSnapshot(Rf::NodeId aId) const;
    std::vector<NodeSnapshot>   GetAllNodeSnapshots(void) const;
    SimNode                    *GetNode(Rf::NodeId aId);

    void     AdvanceUs(uint64_t aDurationUs);
    void     AdvanceSec(double aSeconds);
    void     AdvancePacedUs(uint64_t aDurationUs, double aSpeed);
    void     SetSpeed(double aSpeed) { mConfig.mSpeed = (aSpeed >= 0.0) ? aSpeed : 0.0; }
    double   GetSpeed(void) const { return mConfig.mSpeed; }
    uint64_t GetNowUs(void) const { return mScheduler.GetNowUs(); }

    // Observability: PCAP / WPAN-TAP, KPI, Topology
    bool              StartPcapContained(const std::filesystem::path &aBaseDir,
                                         const std::string           &aRequestedPath,
                                         PcapFormat                   aFormat = PcapFormat::kWpanTap);
    void              StopPcap(void);
    PcapWriter       &GetPcapWriter(void) { return mPcapWriter; }
    KpiTracker       &GetKpiTracker(void) { return mKpiTracker; }
    const KpiTracker &GetKpiTracker(void) const { return mKpiTracker; }

    TopologySpec ExportTopology(void) const;
    bool         LoadTopology(const TopologySpec &aSpec);

    // Virtual Backbone Fabric (TREL / Infra-If / Host UDP)
    NetFabric &GetNetFabric(void) { return mNetFabric; }
    void       SendTrelPacket(const NetPacket &aPacket);
    void       SendInfraIfPacket(const NetPacket &aPacket);
    void       SendHostUdpPacket(const NetPacket &aPacket);

    SimulationStats         GetStats(void) const;
    Scheduler              &GetScheduler(void) { return mScheduler; }
    Rf::RadioModel         &GetRadioModel(void) { return mRadioModel; }
    const Rf::RadioModel   &GetRadioModel(void) const { return mRadioModel; }
    Rf::Prng               &GetPlatformPrng(void) { return mPlatformPrng; }
    const SimulationConfig &GetConfig(void) const { return mConfig; }

    void MarkPendingTasklets(void) { mHasPendingTasklets = true; }

    void StartRadioTx(Rf::NodeId      aSrcId,
                      const RawFrame &aTxFrame,
                      bool            aAckRequested,
                      bool            aCsmaCaEnabled,
                      int8_t          aCcaThresholdDbm,
                      Rf::DbValue     aTxPowerDbm,
                      TxDoneCallback  aOnTxDone);

    void StartEnergyScan(Rf::NodeId             aNodeId,
                         Rf::ChannelId          aChannel,
                         uint64_t               aDurationUs,
                         EnergyScanDoneCallback aOnScanDone);

    static Simulation *GetCurrent(void) { return sCurrentSim; }

private:
    struct ScopedCurrentSim
    {
        explicit ScopedCurrentSim(Simulation *aSim)
            : mPrev(sCurrentSim)
        {
            sCurrentSim = aSim;
        }

        ~ScopedCurrentSim(void) { sCurrentSim = mPrev; }

        Simulation *mPrev;
    };

    void DrainTasklets(void);
    void CompleteRadioTx(Rf::NodeId aSrcId, const RawFrame &aTxFrame, bool aAckRequested, TxDoneCallback aOnTxDone);

    static thread_local Simulation *sCurrentSim;

    SimulationConfig                               mConfig;
    Scheduler                                      mScheduler;
    Rf::RadioModel                                 mRadioModel;
    Rf::Prng                                       mPlatformPrng;
    PcapWriter                                     mPcapWriter;
    KpiTracker                                     mKpiTracker;
    NetFabric                                      mNetFabric;
    Rf::NodeId                                     mNextNodeId         = 1;
    bool                                           mHasPendingTasklets = false;
    uint64_t                                       mStatTxFrames       = 0;
    uint64_t                                       mStatRxOk           = 0;
    uint64_t                                       mStatRxFcsErr       = 0;
    std::map<Rf::NodeId, NodeConfig>               mNodeConfigs;
    std::map<Rf::NodeId, std::unique_ptr<SimNode>> mNodes;
    PacketObserver                                 mPacketObserver;
};

} // namespace Core
} // namespace otns

#endif // OTNS_CORE_SIMULATION_HPP_
