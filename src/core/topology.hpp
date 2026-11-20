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

#ifndef OTNS_CORE_TOPOLOGY_HPP_
#define OTNS_CORE_TOPOLOGY_HPP_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/sim_node.hpp"
#include "nlohmann/json.hpp"
#include "rf/types.hpp"

namespace otns {
namespace Core {

struct TopologyNodeSpec
{
    Rf::NodeId               mId         = Rf::kInvalidNodeId;
    NodeType                 mType       = NodeType::kRouter;
    NodeKind                 mKind       = NodeKind::kInProc;
    Rf::Position3D           mPos        = {};
    double                   mRadioRange = Rf::kDefaultRadioRange;
    bool                     mAutoStart  = true;
    std::string              mExecutable;
    std::vector<std::string> mInitCommands;
};

struct TopologySpec
{
    uint32_t                            mVersion    = 1;
    uint64_t                            mSeed       = 1;
    Rf::RadioModelPreset                mRfPreset   = Rf::RadioModelPreset::kMutualInterference;
    uint16_t                            mPanId      = 0xFACE;
    Rf::ChannelId                       mChannel    = Rf::kMinChannel;
    std::string                         mNetworkKey = "00112233445566778899aabbccddeeff";
    std::optional<Rf::RadioModelParams> mRadioParams;
    std::vector<TopologyNodeSpec>       mNodes;
};

nlohmann::json SerializeTopology(const TopologySpec &aSpec);
bool           DeserializeTopology(const nlohmann::json &aJson, TopologySpec &aOut, std::string &aError);

bool SaveTopologyFileContained(const std::filesystem::path &aBaseDir,
                               const std::string           &aRequestedPath,
                               const TopologySpec          &aSpec);
bool LoadTopologyFileContained(const std::filesystem::path &aBaseDir,
                               const std::string           &aRequestedPath,
                               TopologySpec                &aOut,
                               std::string                 &aError);

} // namespace Core
} // namespace otns

#endif // OTNS_CORE_TOPOLOGY_HPP_
