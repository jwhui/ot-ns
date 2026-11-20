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

#include "core/topology.hpp"

#include <cstdio>

#include "core/path_security.hpp"

namespace otns {
namespace Core {

namespace {

static constexpr size_t kMaxTopologyFileBytes = 16u * 1024u * 1024u;

const char *PresetToString(Rf::RadioModelPreset aPreset)
{
    switch (aPreset)
    {
    case Rf::RadioModelPreset::kIdeal:
        return "Ideal";
    case Rf::RadioModelPreset::kIdealRssi:
        return "IdealRssi";
    case Rf::RadioModelPreset::kMutualInterference:
        return "MutualInterference";
    case Rf::RadioModelPreset::kMidisc:
        return "Midisc";
    case Rf::RadioModelPreset::kOutdoor:
        return "Outdoor";
    }
    return "MutualInterference";
}

bool StringToPreset(const std::string &aStr, Rf::RadioModelPreset &aOut)
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
    if (aStr == "MutualInterference")
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

const char *NodeTypeToString(NodeType aType)
{
    switch (aType)
    {
    case NodeType::kRouter:
        return "router";
    case NodeType::kFed:
        return "fed";
    case NodeType::kMed:
        return "med";
    case NodeType::kSed:
        return "sed";
    }
    return "router";
}

bool StringToNodeType(const std::string &aStr, NodeType &aOut)
{
    if (aStr == "router")
    {
        aOut = NodeType::kRouter;
        return true;
    }
    if (aStr == "fed")
    {
        aOut = NodeType::kFed;
        return true;
    }
    if (aStr == "med")
    {
        aOut = NodeType::kMed;
        return true;
    }
    if (aStr == "sed")
    {
        aOut = NodeType::kSed;
        return true;
    }
    return false;
}

const char *NodeKindToString(NodeKind aKind) { return (aKind == NodeKind::kExternal) ? "external" : "inproc"; }

bool StringToNodeKind(const std::string &aStr, NodeKind &aOut)
{
    if (aStr == "inproc")
    {
        aOut = NodeKind::kInProc;
        return true;
    }
    if (aStr == "external")
    {
        aOut = NodeKind::kExternal;
        return true;
    }
    return false;
}

bool GetDoubleField(const nlohmann::json &aObj, const char *aKey, double &aOut)
{
    if (!aObj.contains(aKey))
    {
        return true;
    }
    if (!aObj[aKey].is_number())
    {
        return false;
    }
    aOut = aObj[aKey].get<double>();
    return true;
}

bool GetBoolField(const nlohmann::json &aObj, const char *aKey, bool &aOut)
{
    if (!aObj.contains(aKey))
    {
        return true;
    }
    if (!aObj[aKey].is_boolean())
    {
        return false;
    }
    aOut = aObj[aKey].get<bool>();
    return true;
}

} // namespace

nlohmann::json SerializeTopology(const TopologySpec &aSpec)
{
    nlohmann::json root  = nlohmann::json::object();
    nlohmann::json nodes = nlohmann::json::array();

    root["version"]     = aSpec.mVersion;
    root["seed"]        = aSpec.mSeed;
    root["rf_preset"]   = PresetToString(aSpec.mRfPreset);
    root["pan_id"]      = aSpec.mPanId;
    root["channel"]     = aSpec.mChannel;
    root["network_key"] = aSpec.mNetworkKey;

    if (aSpec.mRadioParams.has_value())
    {
        const auto    &p  = *aSpec.mRadioParams;
        nlohmann::json rp = nlohmann::json::object();

        rp["meter_per_unit"]          = p.mMeterPerUnit;
        rp["is_disc_limit"]           = p.mIsDiscLimit;
        rp["use_3gpp_pathloss"]       = p.mUse3gppPathloss;
        rp["enable_interference"]     = p.mEnableInterference;
        rp["rssi_min_dbm"]            = p.mRssiMinDbm;
        rp["rssi_max_dbm"]            = p.mRssiMaxDbm;
        rp["exponent_db"]             = p.mExponentDb;
        rp["fixed_loss_db"]           = p.mFixedLossDb;
        rp["nlos_exponent_db"]        = p.mNlosExponentDb;
        rp["nlos_fixed_loss_db"]      = p.mNlosFixedLossDb;
        rp["noise_floor_dbm"]         = p.mNoiseFloorDbm;
        rp["snr_min_threshold_db"]    = p.mSnrMinThresholdDb;
        rp["shadow_fading_sigma_db"]  = p.mShadowFadingSigmaDb;
        rp["time_fading_sigma_max"]   = p.mTimeFadingSigmaMaxDb;
        rp["mean_time_fading_change"] = p.mMeanTimeFadingChange;
        root["radio_params"]          = std::move(rp);
    }

    for (const auto &n : aSpec.mNodes)
    {
        nlohmann::json obj = nlohmann::json::object();

        obj["id"]          = n.mId;
        obj["type"]        = NodeTypeToString(n.mType);
        obj["kind"]        = NodeKindToString(n.mKind);
        obj["x"]           = n.mPos.mX;
        obj["y"]           = n.mPos.mY;
        obj["z"]           = n.mPos.mZ;
        obj["radio_range"] = n.mRadioRange;
        obj["auto_start"]  = n.mAutoStart;

        if (!n.mExecutable.empty())
        {
            obj["executable"] = n.mExecutable;
        }
        if (!n.mInitCommands.empty())
        {
            obj["init_commands"] = n.mInitCommands;
        }
        nodes.push_back(std::move(obj));
    }

    root["nodes"] = std::move(nodes);

    return root;
}

bool DeserializeTopology(const nlohmann::json &aJson, TopologySpec &aOut, std::string &aError)
{
    TopologySpec spec;

    aError.clear();
    if (!aJson.is_object())
    {
        aError = "Topology root must be a JSON object";
        return false;
    }

    if (aJson.contains("version"))
    {
        if (!aJson["version"].is_number_unsigned())
        {
            aError = "Invalid 'version' field";
            return false;
        }
        spec.mVersion = aJson["version"].get<uint32_t>();
        if (spec.mVersion != 1)
        {
            aError = "Unsupported topology schema version";
            return false;
        }
    }

    if (aJson.contains("seed"))
    {
        if (!aJson["seed"].is_number_unsigned())
        {
            aError = "Invalid 'seed' field";
            return false;
        }
        spec.mSeed = aJson["seed"].get<uint64_t>();
    }

    if (aJson.contains("rf_preset"))
    {
        if (!aJson["rf_preset"].is_string() || !StringToPreset(aJson["rf_preset"].get<std::string>(), spec.mRfPreset))
        {
            aError = "Invalid 'rf_preset' field";
            return false;
        }
    }

    if (aJson.contains("pan_id"))
    {
        if (!aJson["pan_id"].is_number_unsigned())
        {
            aError = "Invalid 'pan_id' field";
            return false;
        }
        uint64_t pan = aJson["pan_id"].get<uint64_t>();
        if (pan > 0xFFFFu)
        {
            aError = "'pan_id' out of range";
            return false;
        }
        spec.mPanId = static_cast<uint16_t>(pan);
    }

    if (aJson.contains("channel"))
    {
        if (!aJson["channel"].is_number_unsigned())
        {
            aError = "Invalid 'channel' field";
            return false;
        }
        uint64_t ch = aJson["channel"].get<uint64_t>();
        if (ch < Rf::kMinChannel || ch > Rf::kMaxChannel)
        {
            aError = "'channel' must be in [11, 26]";
            return false;
        }
        spec.mChannel = static_cast<Rf::ChannelId>(ch);
    }

    if (aJson.contains("network_key"))
    {
        if (!aJson["network_key"].is_string())
        {
            aError = "Invalid 'network_key' field";
            return false;
        }
        std::string key = aJson["network_key"].get<std::string>();
        if (key.size() != 32)
        {
            aError = "'network_key' must be 32 hex chars";
            return false;
        }
        spec.mNetworkKey = std::move(key);
    }

    if (aJson.contains("radio_params"))
    {
        const auto &rp = aJson["radio_params"];

        if (!rp.is_object())
        {
            aError = "'radio_params' must be an object";
            return false;
        }

        Rf::RadioModelParams params = Rf::RadioModelParams::FromPreset(spec.mRfPreset);

        if (!GetDoubleField(rp, "meter_per_unit", params.mMeterPerUnit) ||
            !GetBoolField(rp, "is_disc_limit", params.mIsDiscLimit) ||
            !GetBoolField(rp, "use_3gpp_pathloss", params.mUse3gppPathloss) ||
            !GetBoolField(rp, "enable_interference", params.mEnableInterference) ||
            !GetDoubleField(rp, "rssi_min_dbm", params.mRssiMinDbm) ||
            !GetDoubleField(rp, "rssi_max_dbm", params.mRssiMaxDbm) ||
            !GetDoubleField(rp, "exponent_db", params.mExponentDb) ||
            !GetDoubleField(rp, "fixed_loss_db", params.mFixedLossDb) ||
            !GetDoubleField(rp, "nlos_exponent_db", params.mNlosExponentDb) ||
            !GetDoubleField(rp, "nlos_fixed_loss_db", params.mNlosFixedLossDb) ||
            !GetDoubleField(rp, "noise_floor_dbm", params.mNoiseFloorDbm) ||
            !GetDoubleField(rp, "snr_min_threshold_db", params.mSnrMinThresholdDb) ||
            !GetDoubleField(rp, "shadow_fading_sigma_db", params.mShadowFadingSigmaDb) ||
            !GetDoubleField(rp, "time_fading_sigma_max", params.mTimeFadingSigmaMaxDb) ||
            !GetDoubleField(rp, "mean_time_fading_change", params.mMeanTimeFadingChange))
        {
            aError = "Invalid field in 'radio_params'";
            return false;
        }

        spec.mRadioParams = params;
    }

    if (aJson.contains("nodes"))
    {
        const auto &nodes = aJson["nodes"];

        if (!nodes.is_array())
        {
            aError = "'nodes' must be an array";
            return false;
        }

        for (const auto &item : nodes)
        {
            TopologyNodeSpec node;

            if (!item.is_object())
            {
                aError = "Each entry in 'nodes' must be an object";
                return false;
            }

            if (item.contains("id"))
            {
                if (!item["id"].is_number_unsigned())
                {
                    aError = "Invalid node 'id'";
                    return false;
                }
                node.mId = item["id"].get<Rf::NodeId>();
            }

            if (item.contains("type"))
            {
                if (!item["type"].is_string() || !StringToNodeType(item["type"].get<std::string>(), node.mType))
                {
                    aError = "Invalid node 'type'";
                    return false;
                }
            }

            if (item.contains("kind"))
            {
                if (!item["kind"].is_string() || !StringToNodeKind(item["kind"].get<std::string>(), node.mKind))
                {
                    aError = "Invalid node 'kind'";
                    return false;
                }
            }

            if (!GetDoubleField(item, "x", node.mPos.mX) || !GetDoubleField(item, "y", node.mPos.mY) ||
                !GetDoubleField(item, "z", node.mPos.mZ) || !GetDoubleField(item, "radio_range", node.mRadioRange) ||
                !GetBoolField(item, "auto_start", node.mAutoStart))
            {
                aError = "Invalid numeric/boolean node field";
                return false;
            }

            if (item.contains("executable"))
            {
                if (!item["executable"].is_string())
                {
                    aError = "Invalid node 'executable'";
                    return false;
                }
                node.mExecutable = item["executable"].get<std::string>();
            }

            if (item.contains("init_commands"))
            {
                if (!item["init_commands"].is_array())
                {
                    aError = "Invalid node 'init_commands'";
                    return false;
                }
                for (const auto &cmd : item["init_commands"])
                {
                    if (!cmd.is_string())
                    {
                        aError = "Each entry in 'init_commands' must be a string";
                        return false;
                    }
                    node.mInitCommands.push_back(cmd.get<std::string>());
                }
            }

            spec.mNodes.push_back(std::move(node));
        }
    }

    aOut = std::move(spec);

    return true;
}

bool SaveTopologyFileContained(const std::filesystem::path &aBaseDir,
                               const std::string           &aRequestedPath,
                               const TopologySpec          &aSpec)
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

    std::string payload = SerializeTopology(aSpec).dump(2) + "\n";
    std::FILE  *fp      = std::fopen(resolved.c_str(), "wb");

    if (fp == nullptr)
    {
        return false;
    }

    bool ok = (std::fwrite(payload.data(), 1, payload.size(), fp) == payload.size()) && (std::fflush(fp) == 0);

    std::fclose(fp);

    return ok;
}

bool LoadTopologyFileContained(const std::filesystem::path &aBaseDir,
                               const std::string           &aRequestedPath,
                               TopologySpec                &aOut,
                               std::string                 &aError)
{
    std::filesystem::path resolved;

    aError.clear();
    if (!ResolveContainedPath(aBaseDir, aRequestedPath, resolved))
    {
        aError = "Topology path rejected by output directory containment check";
        return false;
    }

    std::error_code ec;
    uintmax_t       fileSize = std::filesystem::file_size(resolved, ec);

    if (ec || fileSize > kMaxTopologyFileBytes)
    {
        aError = "Unable to read topology file or file exceeds size limit";
        return false;
    }

    std::FILE *fp = std::fopen(resolved.c_str(), "rb");

    if (fp == nullptr)
    {
        aError = "Failed to open topology file";
        return false;
    }

    std::string content(static_cast<size_t>(fileSize), '\0');
    size_t      bytesRead = std::fread(content.data(), 1, content.size(), fp);

    std::fclose(fp);

    if (bytesRead != content.size())
    {
        aError = "Short read while loading topology file";
        return false;
    }

    nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);

    if (parsed.is_discarded())
    {
        aError = "Invalid JSON syntax in topology file";
        return false;
    }

    return DeserializeTopology(parsed, aOut, aError);
}

} // namespace Core
} // namespace otns
