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

#include "cli/repl.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "core/path_security.hpp"
#include "core/topology.hpp"

namespace otns {
namespace Cli {

namespace {

const char *RoleName(Core::DeviceRole aRole)
{
    switch (aRole)
    {
    case Core::DeviceRole::kDisabled:
        return "disabled";
    case Core::DeviceRole::kDetached:
        return "detached";
    case Core::DeviceRole::kChild:
        return "child";
    case Core::DeviceRole::kRouter:
        return "router";
    case Core::DeviceRole::kLeader:
        return "leader";
    }
    return "unknown";
}

bool ParseNodeType(const std::string &aStr, Core::NodeType &aOut)
{
    if (aStr == "router" || aStr == "br")
    {
        aOut = Core::NodeType::kRouter;
        return true;
    }
    if (aStr == "fed")
    {
        aOut = Core::NodeType::kFed;
        return true;
    }
    if (aStr == "med")
    {
        aOut = Core::NodeType::kMed;
        return true;
    }
    if (aStr == "sed")
    {
        aOut = Core::NodeType::kSed;
        return true;
    }
    return false;
}

bool ParsePreset(const std::string &aStr, Rf::RadioModelPreset &aOut)
{
    if (aStr == "Ideal")
    {
        aOut = Rf::RadioModelPreset::kIdeal;
        return true;
    }
    if (aStr == "IdealRssi")
    {
        aOut = Rf::RadioModelPreset::kIdealRssi;
        return true;
    }
    if (aStr == "MutualInterference" || aStr == "MI")
    {
        aOut = Rf::RadioModelPreset::kMutualInterference;
        return true;
    }
    if (aStr == "Midisc")
    {
        aOut = Rf::RadioModelPreset::kMidisc;
        return true;
    }
    if (aStr == "Outdoor")
    {
        aOut = Rf::RadioModelPreset::kOutdoor;
        return true;
    }
    return false;
}

} // namespace

Repl::Repl(Core::Simulation            &aSim,
           Server::VizPublisher        &aViz,
           Server::RpcHandler          &aRpc,
           Server::JsonlRecorder       &aRecorder,
           const std::filesystem::path &aOutputDir)
    : mSim(aSim)
    , mViz(aViz)
    , mRpc(aRpc)
    , mRecorder(aRecorder)
    , mOutputDir(aOutputDir)
{
}

bool Repl::ExecuteLine(const std::string &aLine, std::ostream &aOut)
{
    std::istringstream       iss(aLine);
    std::vector<std::string> tokens;
    std::string              tok;

    while (iss >> tok)
    {
        tokens.push_back(tok);
    }

    if (tokens.empty() || tokens[0][0] == '#')
    {
        return true;
    }

    const std::string &cmd = tokens[0];

    if (cmd == "quit" || cmd == "exit")
    {
        mRpc.RequestQuit();
        return false;
    }

    if (cmd == "help" || cmd == "?")
    {
        aOut << "OTNS v3 Interactive Commands:\n"
             << "  add <router|fed|med|sed> [x y [z]]  Add an in-process Thread node\n"
             << "  spawn <exe> [otns|matter|raw] [label] [x y]  Spawn an external ot-rfsim v3 node\n"
             << "  del <id>                            Remove a node\n"
             << "  move <id> <x> <y> [z]               Move a node\n"
             << "  enable|disable <id>                 Enable or disable a node\n"
             << "  go [seconds]                        Advance simulation or run continuously\n"
             << "  pause                               Pause continuous execution\n"
             << "  speed <float|max>                   Set simulation speed (0/max = unpaced)\n"
             << "  node <id> <ot-cli...>               Execute OpenThread CLI on a node\n"
             << "  nodes                               List all nodes\n"
             << "  stats                               Show network role/partition statistics\n"
             << "  kpi [save <path>]                   Show or save JSON KPI summary\n"
             << "  pcap <start <path> [tap|raw]|stop>  Control PCAP/WPAN-TAP capture\n"
             << "  replay <start <path>|stop>          Control JSONL event recording\n"
             << "  save|load <path>                    Save or load JSON topology\n"
             << "  rf <Ideal|IdealRssi|MutualInterference|Midisc|Outdoor>\n"
             << "  reset [seed]                        Reset simulation\n"
             << "  quit                                Exit daemon\n";
        return true;
    }

    if (cmd == "add")
    {
        Core::NodeConfig cfg;

        if (tokens.size() >= 2 && !ParseNodeType(tokens[1], cfg.mType))
        {
            aOut << "Error: unknown node type '" << tokens[1] << "'\n";
            return true;
        }
        if (tokens.size() >= 4)
        {
            cfg.mPos.mX = std::strtod(tokens[2].c_str(), nullptr);
            cfg.mPos.mY = std::strtod(tokens[3].c_str(), nullptr);
        }
        if (tokens.size() >= 5)
        {
            cfg.mPos.mZ = std::strtod(tokens[4].c_str(), nullptr);
        }

        Rf::NodeId id = mSim.AddNode(cfg);

        mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        aOut << id << "\nDone\n";
        return true;
    }

    if (cmd == "spawn" && tokens.size() >= 2)
    {
        Nodes::RfSimServer *rfsim = mRpc.GetRfSimServer();

        if (rfsim == nullptr || !rfsim->IsListening())
        {
            aOut << "Error: start otns with --rfsim-socket or --rfsim-tcp to spawn external nodes\n";
            return true;
        }
        if (mRpc.GetAllowedSpawnPaths().empty())
        {
            aOut << "Error: start otns with --allow-spawn <dir-or-file> to allow spawning external nodes\n";
            return true;
        }

        std::filesystem::path resolvedExe;

        if (!Core::ResolveAllowedExecutable(mRpc.GetAllowedSpawnPaths(), tokens[1], resolvedExe))
        {
            aOut << "Error: executable not permitted by --allow-spawn allowlist\n";
            return true;
        }

        Nodes::SpawnArgStyle argStyle = Nodes::SpawnArgStyle::kOtns;
        Core::NodeConfig     cfg;
        size_t               idx = 2;

        if (idx < tokens.size())
        {
            if (tokens[idx] == "otns")
            {
                argStyle = Nodes::SpawnArgStyle::kOtns;
                ++idx;
            }
            else if (tokens[idx] == "matter")
            {
                argStyle = Nodes::SpawnArgStyle::kMatter;
                ++idx;
            }
            else if (tokens[idx] == "raw")
            {
                argStyle = Nodes::SpawnArgStyle::kRaw;
                ++idx;
            }
        }
        cfg.mAutoStart = (argStyle == Nodes::SpawnArgStyle::kOtns);

        // Optional non-numeric label token before coordinates.
        if (idx < tokens.size())
        {
            char *endPtr = nullptr;

            (void)std::strtod(tokens[idx].c_str(), &endPtr);
            if (endPtr == tokens[idx].c_str() || *endPtr != '\0')
            {
                cfg.mLabel = Core::SanitizeNodeLabel(tokens[idx]);
                ++idx;
            }
        }
        if (idx + 1 < tokens.size())
        {
            cfg.mPos.mX = std::strtod(tokens[idx].c_str(), nullptr);
            cfg.mPos.mY = std::strtod(tokens[idx + 1].c_str(), nullptr);
        }

        Rf::NodeId id = rfsim->SpawnExternalNode(resolvedExe.string(), cfg, {}, 5000, argStyle);

        if (id == Rf::kInvalidNodeId)
        {
            aOut << "Error: spawned process failed to connect to ot-rfsim v3 endpoint\n";
            return true;
        }

        mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        aOut << id << "\nDone\n";
        return true;
    }

    if (cmd == "del" && tokens.size() >= 2)
    {
        Rf::NodeId id = static_cast<Rf::NodeId>(std::strtoul(tokens[1].c_str(), nullptr, 10));

        if (!mSim.RemoveNode(id))
        {
            aOut << "Error: node not found\n";
            return true;
        }
        mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        aOut << "Done\n";
        return true;
    }

    if (cmd == "move" && tokens.size() >= 4)
    {
        Rf::NodeId     id = static_cast<Rf::NodeId>(std::strtoul(tokens[1].c_str(), nullptr, 10));
        Rf::Position3D pos{
            std::strtod(tokens[2].c_str(), nullptr),
            std::strtod(tokens[3].c_str(), nullptr),
            (tokens.size() >= 5) ? std::strtod(tokens[4].c_str(), nullptr) : 0.0,
        };

        if (!mSim.MoveNode(id, pos))
        {
            aOut << "Error: node not found\n";
            return true;
        }
        mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        aOut << "Done\n";
        return true;
    }

    if ((cmd == "enable" || cmd == "disable") && tokens.size() >= 2)
    {
        Rf::NodeId id = static_cast<Rf::NodeId>(std::strtoul(tokens[1].c_str(), nullptr, 10));

        if (!mSim.SetNodeEnabled(id, cmd == "enable"))
        {
            aOut << "Error: node not found\n";
            return true;
        }
        mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        aOut << "Done\n";
        return true;
    }

    if (cmd == "go")
    {
        if (tokens.size() >= 2)
        {
            double sec = std::strtod(tokens[1].c_str(), nullptr);

            mSim.AdvanceSec(sec);
            mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        }
        else
        {
            mRpc.SetPaused(false);
            mViz.PublishDiffIfDue(false, true);
        }
        aOut << "Done\n";
        return true;
    }

    if (cmd == "pause")
    {
        mRpc.SetPaused(true);
        mViz.PublishDiffIfDue(true, true);
        aOut << "Done\n";
        return true;
    }

    if (cmd == "speed" && tokens.size() >= 2)
    {
        double spd = (tokens[1] == "max") ? 0.0 : std::strtod(tokens[1].c_str(), nullptr);

        mSim.SetSpeed(spd);
        mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        aOut << "Done\n";
        return true;
    }

    if (cmd == "node" && tokens.size() >= 3)
    {
        Rf::NodeId  id = static_cast<Rf::NodeId>(std::strtoul(tokens[1].c_str(), nullptr, 10));
        std::string otCmd;

        for (size_t i = 2; i < tokens.size(); ++i)
        {
            if (i > 2)
            {
                otCmd.push_back(' ');
            }
            otCmd += tokens[i];
        }

        aOut << mSim.ExecuteCli(id, otCmd);
        mViz.PublishDiffIfDue(mRpc.IsPaused(), true);
        return true;
    }

    if (cmd == "nodes")
    {
        for (const auto &snap : mSim.GetAllNodeSnapshots())
        {
            char buf[256];

            std::snprintf(buf, sizeof(buf), "id=%u kind=%s role=%s rloc16=0x%04x part=0x%08x pos=(%.1f,%.1f,%.1f)",
                          snap.mId, (snap.mKind == Core::NodeKind::kExternal) ? "ext" : "inproc", RoleName(snap.mRole),
                          static_cast<unsigned>(snap.mRloc16), snap.mPartitionId, snap.mPos.mX, snap.mPos.mY,
                          snap.mPos.mZ);
            aOut << buf;
            if (!snap.mLabel.empty())
            {
                aOut << " label=" << snap.mLabel;
            }
            aOut << "\n";
        }
        aOut << "Done\n";
        return true;
    }

    if (cmd == "stats")
    {
        Core::SimulationStats st = mSim.GetStats();

        aOut << "ts_us=" << mSim.GetNowUs() << " total=" << st.mTotal << " leaders=" << st.mLeaders
             << " routers=" << st.mRouters << " children=" << st.mChildren << " detached=" << st.mDetached
             << " disabled=" << st.mDisabled << " partitions=" << st.mPartitions << " tx=" << st.mTxFrames
             << " rx_ok=" << st.mRxOk << " rx_fcs_err=" << st.mRxFcsErr << "\nDone\n";
        return true;
    }

    if (cmd == "kpi")
    {
        if (tokens.size() >= 3 && tokens[1] == "save")
        {
            if (!mSim.GetKpiTracker().SaveJsonContained(mOutputDir, tokens[2], mSim.GetNowUs()))
            {
                aOut << "Error: failed to save KPI inside output directory\n";
                return true;
            }
            aOut << "Done\n";
            return true;
        }
        aOut << mSim.GetKpiTracker().ToJson(mSim.GetNowUs()).dump(2) << "\nDone\n";
        return true;
    }

    if (cmd == "pcap" && tokens.size() >= 2)
    {
        if (tokens[1] == "start" && tokens.size() >= 3)
        {
            Core::PcapFormat fmt =
                (tokens.size() >= 4 && tokens[3] == "raw") ? Core::PcapFormat::kIeee802154 : Core::PcapFormat::kWpanTap;

            if (!mSim.StartPcapContained(mOutputDir, tokens[2], fmt))
            {
                aOut << "Error: failed to start PCAP capture\n";
                return true;
            }
            aOut << "Done\n";
            return true;
        }
        if (tokens[1] == "stop")
        {
            mSim.StopPcap();
            aOut << "Done\n";
            return true;
        }
    }

    if (cmd == "replay" && tokens.size() >= 2)
    {
        if (tokens[1] == "start" && tokens.size() >= 3)
        {
            if (!mRecorder.OpenContained(mOutputDir, tokens[2]))
            {
                aOut << "Error: failed to start replay recording\n";
                return true;
            }
            (void)mRecorder.WriteEvent(mViz.BuildSnapshot(mRpc.IsPaused()).dump());
            aOut << "Done\n";
            return true;
        }
        if (tokens[1] == "stop")
        {
            mRecorder.Close();
            aOut << "Done\n";
            return true;
        }
    }

    if (cmd == "save" && tokens.size() >= 2)
    {
        if (!Core::SaveTopologyFileContained(mOutputDir, tokens[1], mSim.ExportTopology()))
        {
            aOut << "Error: failed to save topology\n";
            return true;
        }
        aOut << "Done\n";
        return true;
    }

    if (cmd == "load" && tokens.size() >= 2)
    {
        Core::TopologySpec spec;
        std::string        err;

        if (!Core::LoadTopologyFileContained(mOutputDir, tokens[1], spec, err))
        {
            aOut << "Error: " << err << "\n";
            return true;
        }
        (void)mSim.LoadTopology(spec);
        mViz.PublishReset(mRpc.IsPaused());
        aOut << "Done\n";
        return true;
    }

    if (cmd == "rf" && tokens.size() >= 2)
    {
        Rf::RadioModelPreset preset;

        if (!ParsePreset(tokens[1], preset))
        {
            aOut << "Error: unknown RF preset\n";
            return true;
        }
        mSim.SetRfPreset(preset);
        mViz.PublishReset(mRpc.IsPaused());
        aOut << "Done\n";
        return true;
    }

    if (cmd == "reset")
    {
        std::optional<uint64_t> seed;

        if (tokens.size() >= 2)
        {
            seed = static_cast<uint64_t>(std::strtoull(tokens[1].c_str(), nullptr, 10));
        }
        mSim.Reset(seed);
        mRpc.SetPaused(true);
        mViz.PublishReset(true);
        aOut << "Done\n";
        return true;
    }

    aOut << "Error: unknown command '" << cmd << "' (type 'help')\n";
    return true;
}

} // namespace Cli
} // namespace otns
