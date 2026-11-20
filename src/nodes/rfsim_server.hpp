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

#ifndef OTNS_NODES_RFSIM_SERVER_HPP_
#define OTNS_NODES_RFSIM_SERVER_HPP_

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/sim_node.hpp"
#include "rf/types.hpp"

namespace otns {
namespace Core {
class Simulation;
} // namespace Core

namespace Nodes {

enum class SpawnArgStyle : uint8_t
{
    kOtns   = 0, // Passes --endpoint <uri> --node-id <id> [--label <lbl>] + extra args
    kMatter = 1, // Passes --thread-args=<id> --thread-args=<sock> [--thread-args=<seed>] + extra args
    kRaw    = 2, // Passes extra args directly with {endpoint}, {socket}, {node_id}, {seed} substitution
};

/**
 * POSIX poll()-based ot-rfsim v3 socket listener supporting both Unix domain
 * sockets (AF_UNIX) and TCP sockets (default loopback 127.0.0.1).
 */
class RfSimServer
{
public:
    explicit RfSimServer(Core::Simulation &aSim);
    ~RfSimServer(void);

    RfSimServer(const RfSimServer &)            = delete;
    RfSimServer &operator=(const RfSimServer &) = delete;

    bool ListenUnix(const std::string &aSocketPath);
    bool ListenTcp(uint16_t aPort = 0, const std::string &aBindAddr = "127.0.0.1");
    void Close(void);

    bool        IsListening(void) const { return mListenFd >= 0; }
    uint16_t    GetTcpPort(void) const { return mTcpPort; }
    std::string GetEndpoint(void) const { return mEndpoint; }

    /**
     * Waits up to @p aTimeoutMs for an external ot-rfsim v3 peer to connect,
     * validates Hello/HelloAck, registers it into Simulation, and syncs startup.
     */
    Rf::NodeId AcceptExternalNode(const Core::NodeConfig &aConfig, int aTimeoutMs = 5000);

    /**
     * Spawns an external ot-rfsim v3 binary via fork/execv (no shell invocation),
     * connects it to this listener, and registers it into Simulation.
     */
    Rf::NodeId SpawnExternalNode(const std::string              &aExecutable,
                                 const Core::NodeConfig         &aConfig,
                                 const std::vector<std::string> &aExtraArgs = {},
                                 int                             aTimeoutMs = 5000,
                                 SpawnArgStyle                   aArgStyle  = SpawnArgStyle::kOtns);

private:
    Rf::NodeId HandshakeAndRegister(int aClientFd, const Core::NodeConfig &aConfig, pid_t aChildPid, int aTimeoutMs);

    Core::Simulation &mSim;
    int               mListenFd = -1;
    uint16_t          mTcpPort  = 0;
    std::string       mUnixPath;
    std::string       mEndpoint;
};

} // namespace Nodes
} // namespace otns

#endif // OTNS_NODES_RFSIM_SERVER_HPP_
