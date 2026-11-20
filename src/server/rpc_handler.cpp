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

#include "server/rpc_handler.hpp"

#include <algorithm>
#include <cmath>

#include "core/path_security.hpp"
#include "core/topology.hpp"

namespace otns {
namespace Server {

namespace {

bool ParseNodeType(const std::string &aName, Core::NodeType &aOut)
{
    if (aName == "router" || aName == "br")
    {
        aOut = Core::NodeType::kRouter;
        return true;
    }
    if (aName == "fed")
    {
        aOut = Core::NodeType::kFed;
        return true;
    }
    if (aName == "med")
    {
        aOut = Core::NodeType::kMed;
        return true;
    }
    if (aName == "sed")
    {
        aOut = Core::NodeType::kSed;
        return true;
    }
    return false;
}

bool ParsePreset(const std::string &aName, Rf::RadioModelPreset &aOut)
{
    if (aName == "Ideal")
    {
        aOut = Rf::RadioModelPreset::kIdeal;
        return true;
    }
    if (aName == "IdealRssi")
    {
        aOut = Rf::RadioModelPreset::kIdealRssi;
        return true;
    }
    if (aName == "MutualInterference" || aName == "MI")
    {
        aOut = Rf::RadioModelPreset::kMutualInterference;
        return true;
    }
    if (aName == "Midisc")
    {
        aOut = Rf::RadioModelPreset::kMidisc;
        return true;
    }
    if (aName == "Outdoor")
    {
        aOut = Rf::RadioModelPreset::kOutdoor;
        return true;
    }
    return false;
}

bool ParseSpawnArgStyle(const std::string &aName, Nodes::SpawnArgStyle &aOut)
{
    if (aName == "otns")
    {
        aOut = Nodes::SpawnArgStyle::kOtns;
        return true;
    }
    if (aName == "matter")
    {
        aOut = Nodes::SpawnArgStyle::kMatter;
        return true;
    }
    if (aName == "raw")
    {
        aOut = Nodes::SpawnArgStyle::kRaw;
        return true;
    }
    return false;
}

nlohmann::json ParamsToJson(const Rf::RadioModelParams &aParams)
{
    nlohmann::json p = nlohmann::json::object();

    p["meter_per_unit"]          = aParams.mMeterPerUnit;
    p["is_disc_limit"]           = aParams.mIsDiscLimit;
    p["use_3gpp_pathloss"]       = aParams.mUse3gppPathloss;
    p["enable_interference"]     = aParams.mEnableInterference;
    p["rssi_min_dbm"]            = aParams.mRssiMinDbm;
    p["rssi_max_dbm"]            = aParams.mRssiMaxDbm;
    p["exponent_db"]             = aParams.mExponentDb;
    p["fixed_loss_db"]           = aParams.mFixedLossDb;
    p["nlos_exponent_db"]        = aParams.mNlosExponentDb;
    p["nlos_fixed_loss_db"]      = aParams.mNlosFixedLossDb;
    p["noise_floor_dbm"]         = aParams.mNoiseFloorDbm;
    p["snr_min_threshold_db"]    = aParams.mSnrMinThresholdDb;
    p["shadow_fading_sigma_db"]  = aParams.mShadowFadingSigmaDb;
    p["time_fading_sigma_max"]   = aParams.mTimeFadingSigmaMaxDb;
    p["mean_time_fading_change"] = aParams.mMeanTimeFadingChange;

    return p;
}

} // namespace

RpcHandler::RpcHandler(Core::Simulation            &aSim,
                       WsServer                    &aServer,
                       VizPublisher                &aViz,
                       JsonlRecorder               &aRecorder,
                       const std::filesystem::path &aOutputDir,
                       Nodes::RfSimServer          *aRfSimServer)
    : mSim(aSim)
    , mServer(aServer)
    , mViz(aViz)
    , mRecorder(aRecorder)
    , mOutputDir(aOutputDir)
    , mRfSimServer(aRfSimServer)
{
}

void RpcHandler::AddAllowedSpawnPath(const std::filesystem::path &aPath)
{
    if (!aPath.empty())
    {
        mAllowedSpawnPaths.push_back(aPath);
    }
}

nlohmann::json RpcHandler::MakeResult(const nlohmann::json &aId, nlohmann::json aResult)
{
    nlohmann::json res = nlohmann::json::object();

    res["v"]      = 1;
    res["type"]   = "rpc.res";
    res["id"]     = aId;
    res["result"] = std::move(aResult);

    return res;
}

nlohmann::json RpcHandler::MakeError(const nlohmann::json &aId, const char *aCode, const std::string &aMessage)
{
    nlohmann::json res = nlohmann::json::object();
    nlohmann::json err = nlohmann::json::object();

    err["code"]    = aCode;
    err["message"] = aMessage;

    res["v"]     = 1;
    res["type"]  = "rpc.res";
    res["id"]    = aId;
    res["error"] = std::move(err);

    return res;
}

void RpcHandler::HandleMessage(ClientId aClientId, const std::string &aMessage)
{
    nlohmann::json req = nlohmann::json::parse(aMessage, nullptr, false);

    if (req.is_discarded() || !req.is_object())
    {
        (void)mServer.SendText(aClientId, MakeError(nullptr, "PARSE_ERROR", "Invalid JSON message").dump());
        return;
    }

    nlohmann::json id = req.contains("id") ? req["id"] : nlohmann::json(0);

    if (!req.contains("v") || req["v"] != 1 || !req.contains("type") || req["type"] != "rpc.req" ||
        !req.contains("method") || !req["method"].is_string())
    {
        (void)mServer.SendText(aClientId, MakeError(id, "INVALID_REQUEST", "Expected v1 rpc.req with method").dump());
        return;
    }

    std::string    method = req["method"].get<std::string>();
    nlohmann::json params =
        (req.contains("params") && req["params"].is_object()) ? req["params"] : nlohmann::json::object();
    nlohmann::json out = Dispatch(aClientId, method, params);

    if (out.contains("__rpc_error_code"))
    {
        std::string code = out["__rpc_error_code"].get<std::string>();
        std::string msg  = out["__rpc_error_msg"].get<std::string>();

        (void)mServer.SendText(aClientId, MakeError(id, code.c_str(), msg).dump());
    }
    else
    {
        (void)mServer.SendText(aClientId, MakeResult(id, std::move(out)).dump());
    }
}

nlohmann::json RpcHandler::Dispatch(ClientId aClientId, const std::string &aMethod, const nlohmann::json &aParams)
{
    auto err = [](const char *aCode, const std::string &aMsg) {
        nlohmann::json e = nlohmann::json::object();

        e["__rpc_error_code"] = aCode;
        e["__rpc_error_msg"]  = aMsg;
        return e;
    };

    if (aMethod == "sim.info")
    {
        nlohmann::json res = nlohmann::json::object();

        res["engine"]        = "otns-v3";
        res["version"]       = 1;
        res["ts_us"]         = mSim.GetNowUs();
        res["speed"]         = mSim.GetSpeed();
        res["paused"]        = mPaused;
        res["node_count"]    = mSim.GetStats().mTotal;
        res["spawn_enabled"] = !mAllowedSpawnPaths.empty() && (mRfSimServer != nullptr) && mRfSimServer->IsListening();
        if (mRfSimServer != nullptr && mRfSimServer->IsListening())
        {
            res["rfsim_endpoint"] = mRfSimServer->GetEndpoint();
        }
        return res;
    }

    if (aMethod == "sim.go")
    {
        if (aParams.contains("speed") && aParams["speed"].is_number())
        {
            mSim.SetSpeed(aParams["speed"].get<double>());
        }

        if (aParams.contains("duration_us") && aParams["duration_us"].is_number_unsigned())
        {
            uint64_t durUs = aParams["duration_us"].get<uint64_t>();
            double   spd   = mSim.GetSpeed();

            if (spd > 0.0)
            {
                mSim.AdvancePacedUs(durUs, spd);
            }
            else
            {
                mSim.AdvanceUs(durUs);
            }
            mViz.PublishDiffIfDue(mPaused, true);
        }
        else if (aParams.contains("duration_sec") && aParams["duration_sec"].is_number())
        {
            mSim.AdvanceSec(aParams["duration_sec"].get<double>());
            mViz.PublishDiffIfDue(mPaused, true);
        }
        else
        {
            mPaused = false;
            mViz.PublishDiffIfDue(mPaused, true);
        }

        nlohmann::json res = nlohmann::json::object();

        res["ts_us"]  = mSim.GetNowUs();
        res["paused"] = mPaused;
        return res;
    }

    if (aMethod == "sim.pause")
    {
        mPaused = true;
        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"paused", true}, {"ts_us", mSim.GetNowUs()}};
    }

    if (aMethod == "sim.resume")
    {
        mPaused = false;
        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"paused", false}, {"ts_us", mSim.GetNowUs()}};
    }

    if (aMethod == "sim.set_speed")
    {
        if (!aParams.contains("speed") || !aParams["speed"].is_number())
        {
            return err("INVALID_PARAMS", "Expected numeric 'speed'");
        }
        double speed = aParams["speed"].get<double>();

        if (speed < 0.0 || !std::isfinite(speed))
        {
            return err("INVALID_PARAMS", "'speed' must be finite and >= 0");
        }
        mSim.SetSpeed(speed);
        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"speed", mSim.GetSpeed()}};
    }

    if (aMethod == "sim.reset")
    {
        std::optional<uint64_t> seed;

        if (aParams.contains("seed") && aParams["seed"].is_number_unsigned())
        {
            seed = aParams["seed"].get<uint64_t>();
        }
        mSim.Reset(seed);
        mPaused = true;
        mViz.PublishReset(mPaused);
        return nlohmann::json{{"ts_us", mSim.GetNowUs()}, {"seed", mSim.GetConfig().mSeed}};
    }

    if (aMethod == "node.add")
    {
        Core::NodeConfig cfg;

        if (aParams.contains("type"))
        {
            if (!aParams["type"].is_string() || !ParseNodeType(aParams["type"].get<std::string>(), cfg.mType))
            {
                return err("INVALID_PARAMS", "Unknown node 'type'");
            }
        }
        else if (aParams.contains("role"))
        {
            if (!aParams["role"].is_string() || !ParseNodeType(aParams["role"].get<std::string>(), cfg.mType))
            {
                return err("INVALID_PARAMS", "Unknown node 'role'");
            }
        }

        if (aParams.contains("x") && aParams["x"].is_number())
        {
            cfg.mPos.mX = aParams["x"].get<double>();
        }
        if (aParams.contains("y") && aParams["y"].is_number())
        {
            cfg.mPos.mY = aParams["y"].get<double>();
        }
        if (aParams.contains("z") && aParams["z"].is_number())
        {
            cfg.mPos.mZ = aParams["z"].get<double>();
        }
        if (aParams.contains("radio_range") && aParams["radio_range"].is_number())
        {
            cfg.mRadioRange = aParams["radio_range"].get<double>();
        }
        if (aParams.contains("auto_start") && aParams["auto_start"].is_boolean())
        {
            cfg.mAutoStart = aParams["auto_start"].get<bool>();
        }
        if (aParams.contains("label") && aParams["label"].is_string())
        {
            cfg.mLabel = Core::SanitizeNodeLabel(aParams["label"].get<std::string>());
        }

        Rf::NodeId id = mSim.AddNode(cfg);

        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"node_id", id}};
    }

    if (aMethod == "node.spawn")
    {
        static constexpr size_t kMaxSpawnArgs      = 32;
        static constexpr size_t kMaxSpawnArgBytes  = 4096;
        static constexpr int    kMinSpawnTimeoutMs = 500;
        static constexpr int    kMaxSpawnTimeoutMs = 30000;

        if (mRfSimServer == nullptr || !mRfSimServer->IsListening())
        {
            return err("RFSIM_DISABLED", "Start otns with --rfsim-socket or --rfsim-tcp to spawn external nodes");
        }
        if (mAllowedSpawnPaths.empty())
        {
            return err("SPAWN_DISABLED",
                       "External node spawning is disabled; start otns with --allow-spawn <dir-or-file>");
        }
        if (!aParams.contains("executable") || !aParams["executable"].is_string())
        {
            return err("INVALID_PARAMS", "Expected string 'executable'");
        }

        std::filesystem::path resolvedExe;
        std::string           reqExe = aParams["executable"].get<std::string>();

        if (!Core::ResolveAllowedExecutable(mAllowedSpawnPaths, reqExe, resolvedExe))
        {
            return err("FORBIDDEN_EXECUTABLE",
                       "Executable must be a regular executable file inside an allowlisted --allow-spawn path");
        }

        Nodes::SpawnArgStyle argStyle = Nodes::SpawnArgStyle::kOtns;

        if (aParams.contains("arg_style"))
        {
            if (!aParams["arg_style"].is_string() ||
                !ParseSpawnArgStyle(aParams["arg_style"].get<std::string>(), argStyle))
            {
                return err("INVALID_PARAMS", "Unknown 'arg_style' (expected 'otns', 'matter', or 'raw')");
            }
        }

        Core::NodeConfig cfg;

        cfg.mAutoStart = (argStyle == Nodes::SpawnArgStyle::kOtns);
        if (aParams.contains("type"))
        {
            if (!aParams["type"].is_string() || !ParseNodeType(aParams["type"].get<std::string>(), cfg.mType))
            {
                return err("INVALID_PARAMS", "Unknown node 'type'");
            }
        }
        else if (aParams.contains("role"))
        {
            if (!aParams["role"].is_string() || !ParseNodeType(aParams["role"].get<std::string>(), cfg.mType))
            {
                return err("INVALID_PARAMS", "Unknown node 'role'");
            }
        }
        if (aParams.contains("x") && aParams["x"].is_number())
        {
            cfg.mPos.mX = aParams["x"].get<double>();
        }
        if (aParams.contains("y") && aParams["y"].is_number())
        {
            cfg.mPos.mY = aParams["y"].get<double>();
        }
        if (aParams.contains("z") && aParams["z"].is_number())
        {
            cfg.mPos.mZ = aParams["z"].get<double>();
        }
        if (aParams.contains("radio_range") && aParams["radio_range"].is_number())
        {
            cfg.mRadioRange = aParams["radio_range"].get<double>();
        }
        if (aParams.contains("auto_start") && aParams["auto_start"].is_boolean())
        {
            cfg.mAutoStart = aParams["auto_start"].get<bool>();
        }
        if (aParams.contains("label") && aParams["label"].is_string())
        {
            cfg.mLabel = Core::SanitizeNodeLabel(aParams["label"].get<std::string>());
        }

        std::vector<std::string> extraArgs;

        if (aParams.contains("args"))
        {
            if (!aParams["args"].is_array() || aParams["args"].size() > kMaxSpawnArgs)
            {
                return err("INVALID_PARAMS", "'args' must be an array of at most 32 strings");
            }
            for (const auto &v : aParams["args"])
            {
                if (!v.is_string())
                {
                    return err("INVALID_PARAMS", "Each entry in 'args' must be a string");
                }
                std::string s = v.get<std::string>();

                if (s.size() > kMaxSpawnArgBytes || s.find('\0') != std::string::npos)
                {
                    return err("INVALID_PARAMS", "Invalid argument string in 'args'");
                }
                extraArgs.push_back(std::move(s));
            }
        }

        int timeoutMs = 5000;

        if (aParams.contains("timeout_ms") && aParams["timeout_ms"].is_number_unsigned())
        {
            timeoutMs = static_cast<int>(
                std::clamp<uint64_t>(aParams["timeout_ms"].get<uint64_t>(), kMinSpawnTimeoutMs, kMaxSpawnTimeoutMs));
        }

        Rf::NodeId id = mRfSimServer->SpawnExternalNode(resolvedExe.string(), cfg, extraArgs, timeoutMs, argStyle);

        if (id == Rf::kInvalidNodeId)
        {
            return err("SPAWN_FAILED", "Spawned process failed to complete ot-rfsim v3 handshake within timeout");
        }

        auto           snap = mSim.GetNodeSnapshot(id);
        nlohmann::json res  = nlohmann::json::object();

        res["node_id"]  = id;
        res["kind"]     = "external";
        res["endpoint"] = mRfSimServer->GetEndpoint();
        if (snap.has_value() && !snap->mLabel.empty())
        {
            res["label"] = snap->mLabel;
        }

        mViz.PublishDiffIfDue(mPaused, true);
        return res;
    }

    if (aMethod == "node.remove")
    {
        if (!aParams.contains("node_id") || !aParams["node_id"].is_number_unsigned())
        {
            return err("INVALID_PARAMS", "Expected unsigned 'node_id'");
        }
        Rf::NodeId id = aParams["node_id"].get<Rf::NodeId>();

        if (!mSim.RemoveNode(id))
        {
            return err("NOT_FOUND", "Node not found");
        }
        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"removed", id}};
    }

    if (aMethod == "node.move")
    {
        if (!aParams.contains("node_id") || !aParams["node_id"].is_number_unsigned() || !aParams.contains("x") ||
            !aParams["x"].is_number() || !aParams.contains("y") || !aParams["y"].is_number())
        {
            return err("INVALID_PARAMS", "Expected 'node_id', 'x', 'y'");
        }
        Rf::NodeId     id = aParams["node_id"].get<Rf::NodeId>();
        Rf::Position3D pos{
            aParams["x"].get<double>(),
            aParams["y"].get<double>(),
            (aParams.contains("z") && aParams["z"].is_number()) ? aParams["z"].get<double>() : 0.0,
        };

        if (!mSim.MoveNode(id, pos))
        {
            return err("NOT_FOUND", "Node not found");
        }
        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"node_id", id}, {"x", pos.mX}, {"y", pos.mY}, {"z", pos.mZ}};
    }

    if (aMethod == "node.set_enabled")
    {
        if (!aParams.contains("node_id") || !aParams["node_id"].is_number_unsigned() || !aParams.contains("enabled") ||
            !aParams["enabled"].is_boolean())
        {
            return err("INVALID_PARAMS", "Expected 'node_id' and boolean 'enabled'");
        }
        Rf::NodeId id      = aParams["node_id"].get<Rf::NodeId>();
        bool       enabled = aParams["enabled"].get<bool>();

        if (!mSim.SetNodeEnabled(id, enabled))
        {
            return err("NOT_FOUND", "Node not found");
        }
        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"node_id", id}, {"enabled", enabled}};
    }

    if (aMethod == "node.cli")
    {
        if (!aParams.contains("node_id") || !aParams["node_id"].is_number_unsigned() || !aParams.contains("command") ||
            !aParams["command"].is_string())
        {
            return err("INVALID_PARAMS", "Expected 'node_id' and string 'command'");
        }
        Rf::NodeId  id  = aParams["node_id"].get<Rf::NodeId>();
        std::string cmd = aParams["command"].get<std::string>();

        if (mSim.GetNode(id) == nullptr)
        {
            return err("NOT_FOUND", "Node not found");
        }

        std::string output = mSim.ExecuteCli(id, cmd);

        mViz.PublishDiffIfDue(mPaused, true);
        return nlohmann::json{{"node_id", id}, {"output", output}};
    }

    if (aMethod == "node.take_cli_output")
    {
        if (!aParams.contains("node_id") || !aParams["node_id"].is_number_unsigned())
        {
            return err("INVALID_PARAMS", "Expected 'node_id'");
        }
        Rf::NodeId id = aParams["node_id"].get<Rf::NodeId>();

        return nlohmann::json{{"node_id", id}, {"output", mSim.TakeCliOutput(id)}};
    }

    if (aMethod == "node.list")
    {
        return mViz.BuildSnapshot(mPaused);
    }

    if (aMethod == "rf.get_params")
    {
        return ParamsToJson(mSim.GetRadioModel().GetParams());
    }

    if (aMethod == "rf.set_params")
    {
        bool presetChanged = false;

        if (aParams.contains("model") && aParams["model"].is_string())
        {
            Rf::RadioModelPreset preset;

            if (!ParsePreset(aParams["model"].get<std::string>(), preset))
            {
                return err("INVALID_PARAMS", "Unknown RF model preset");
            }
            mSim.SetRfPreset(preset);
            presetChanged = true;
        }

        Rf::RadioModelParams p = mSim.GetRadioModel().GetParams();

        if (aParams.contains("noise_floor_dbm") && aParams["noise_floor_dbm"].is_number())
        {
            p.mNoiseFloorDbm = aParams["noise_floor_dbm"].get<double>();
        }
        if (aParams.contains("shadow_fading_sigma_db") && aParams["shadow_fading_sigma_db"].is_number())
        {
            p.mShadowFadingSigmaDb = aParams["shadow_fading_sigma_db"].get<double>();
        }
        if (aParams.contains("time_fading_sigma_max") && aParams["time_fading_sigma_max"].is_number())
        {
            p.mTimeFadingSigmaMaxDb = aParams["time_fading_sigma_max"].get<double>();
        }
        if (aParams.contains("meter_per_unit") && aParams["meter_per_unit"].is_number())
        {
            p.mMeterPerUnit = aParams["meter_per_unit"].get<double>();
        }
        if (aParams.contains("is_disc_limit") && aParams["is_disc_limit"].is_boolean())
        {
            p.mIsDiscLimit = aParams["is_disc_limit"].get<bool>();
        }

        mSim.GetRadioModel().SetParams(p);
        if (presetChanged)
        {
            mViz.PublishReset(mPaused);
        }
        return ParamsToJson(mSim.GetRadioModel().GetParams());
    }

    if (aMethod == "viz.subscribe")
    {
        VizSubscription sub;

        if (aParams.contains("max_hz") && aParams["max_hz"].is_number_unsigned())
        {
            sub.mMaxHz = aParams["max_hz"].get<uint32_t>();
        }
        if (aParams.contains("packets") && aParams["packets"].is_boolean())
        {
            sub.mPackets = aParams["packets"].get<bool>();
        }
        if (aParams.contains("watch_nodes") && aParams["watch_nodes"].is_array())
        {
            for (const auto &v : aParams["watch_nodes"])
            {
                if (v.is_number_unsigned())
                {
                    sub.mWatchNodes.insert(v.get<Rf::NodeId>());
                }
            }
        }
        mViz.UpdateSubscription(aClientId, sub);
        return nlohmann::json{{"subscribed", true}};
    }

    if (aMethod == "kpi.get")
    {
        return mSim.GetKpiTracker().ToJson(mSim.GetNowUs());
    }

    if (aMethod == "kpi.save")
    {
        if (!aParams.contains("path") || !aParams["path"].is_string())
        {
            return err("INVALID_PARAMS", "Expected string 'path'");
        }
        std::string reqPath = aParams["path"].get<std::string>();

        if (!mSim.GetKpiTracker().SaveJsonContained(mOutputDir, reqPath, mSim.GetNowUs()))
        {
            return err("FORBIDDEN_PATH", "Failed to save KPI report inside output directory");
        }
        return nlohmann::json{{"saved", reqPath}};
    }

    if (aMethod == "pcap.start")
    {
        if (!aParams.contains("path") || !aParams["path"].is_string())
        {
            return err("INVALID_PARAMS", "Expected string 'path'");
        }
        std::string      reqPath = aParams["path"].get<std::string>();
        Core::PcapFormat fmt     = Core::PcapFormat::kWpanTap;

        if (aParams.contains("format") && aParams["format"].is_string() &&
            aParams["format"].get<std::string>() == "ieee802154")
        {
            fmt = Core::PcapFormat::kIeee802154;
        }
        if (!mSim.StartPcapContained(mOutputDir, reqPath, fmt))
        {
            return err("FORBIDDEN_PATH", "Failed to start PCAP inside output directory");
        }
        return nlohmann::json{{"recording", true}, {"path", reqPath}};
    }

    if (aMethod == "pcap.stop")
    {
        mSim.StopPcap();
        return nlohmann::json{{"recording", false}, {"frames", mSim.GetPcapWriter().GetFrameCount()}};
    }

    if (aMethod == "topology.export")
    {
        return Core::SerializeTopology(mSim.ExportTopology());
    }

    if (aMethod == "topology.import")
    {
        Core::TopologySpec spec;
        std::string        parseErr;

        if (!Core::DeserializeTopology(aParams, spec, parseErr))
        {
            return err("INVALID_PARAMS", parseErr);
        }
        (void)mSim.LoadTopology(spec);
        mViz.PublishReset(mPaused);
        return nlohmann::json{{"imported_nodes", mSim.GetStats().mTotal}};
    }

    if (aMethod == "topology.save")
    {
        if (!aParams.contains("path") || !aParams["path"].is_string())
        {
            return err("INVALID_PARAMS", "Expected string 'path'");
        }
        std::string reqPath = aParams["path"].get<std::string>();

        if (!Core::SaveTopologyFileContained(mOutputDir, reqPath, mSim.ExportTopology()))
        {
            return err("FORBIDDEN_PATH", "Failed to save topology inside output directory");
        }
        return nlohmann::json{{"saved", reqPath}};
    }

    if (aMethod == "topology.load")
    {
        if (!aParams.contains("path") || !aParams["path"].is_string())
        {
            return err("INVALID_PARAMS", "Expected string 'path'");
        }
        std::string        reqPath = aParams["path"].get<std::string>();
        Core::TopologySpec spec;
        std::string        loadErr;

        if (!Core::LoadTopologyFileContained(mOutputDir, reqPath, spec, loadErr))
        {
            return err("FORBIDDEN_PATH", loadErr);
        }
        (void)mSim.LoadTopology(spec);
        mViz.PublishReset(mPaused);
        return nlohmann::json{{"loaded_nodes", mSim.GetStats().mTotal}};
    }

    if (aMethod == "replay.start")
    {
        if (!aParams.contains("path") || !aParams["path"].is_string())
        {
            return err("INVALID_PARAMS", "Expected string 'path'");
        }
        std::string reqPath = aParams["path"].get<std::string>();

        if (!mRecorder.OpenContained(mOutputDir, reqPath))
        {
            return err("FORBIDDEN_PATH", "Failed to open replay file inside output directory");
        }
        (void)mRecorder.WriteEvent(mViz.BuildSnapshot(mPaused).dump());
        return nlohmann::json{{"recording", true}, {"path", reqPath}};
    }

    if (aMethod == "replay.stop")
    {
        uint64_t count = mRecorder.GetEventCount();

        mRecorder.Close();
        return nlohmann::json{{"recording", false}, {"events", count}};
    }

    return err("METHOD_NOT_FOUND", "Unknown RPC method: " + aMethod);
}

void RpcHandler::StepContinuous(uint64_t aSliceUs)
{
    if (mPaused || aSliceUs == 0)
    {
        return;
    }

    mSim.AdvanceUs(aSliceUs);
    mViz.PublishDiffIfDue(mPaused, false);
}

} // namespace Server
} // namespace otns
