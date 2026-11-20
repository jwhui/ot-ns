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

#ifndef OTNS_SERVER_RPC_HANDLER_HPP_
#define OTNS_SERVER_RPC_HANDLER_HPP_

#include <filesystem>
#include <string>
#include <vector>

#include "core/simulation.hpp"
#include "nlohmann/json.hpp"
#include "nodes/rfsim_server.hpp"
#include "server/replay.hpp"
#include "server/viz_publisher.hpp"
#include "server/ws_server.hpp"

namespace otns {
namespace Server {

/**
 * Handles v1 JSON-RPC control requests (viz_rpc_v1.schema.json) from WebSocket
 * and Unix RPC clients and drives Simulation + VizPublisher + JsonlRecorder.
 */
class RpcHandler
{
public:
    RpcHandler(Core::Simulation            &aSim,
               WsServer                    &aServer,
               VizPublisher                &aViz,
               JsonlRecorder               &aRecorder,
               const std::filesystem::path &aOutputDir,
               Nodes::RfSimServer          *aRfSimServer = nullptr);

    void HandleMessage(ClientId aClientId, const std::string &aMessage);

    void                                      AddAllowedSpawnPath(const std::filesystem::path &aPath);
    const std::vector<std::filesystem::path> &GetAllowedSpawnPaths(void) const { return mAllowedSpawnPaths; }
    Nodes::RfSimServer                       *GetRfSimServer(void) const { return mRfSimServer; }

    bool IsPaused(void) const { return mPaused; }
    void SetPaused(bool aPaused) { mPaused = aPaused; }
    bool IsQuitRequested(void) const { return mQuitRequested; }
    void RequestQuit(void) { mQuitRequested = true; }

    /**
     * Advances simulation slightly when unpaused in continuous run mode.
     */
    void StepContinuous(uint64_t aSliceUs);

private:
    nlohmann::json        Dispatch(ClientId aClientId, const std::string &aMethod, const nlohmann::json &aParams);
    static nlohmann::json MakeResult(const nlohmann::json &aId, nlohmann::json aResult);
    static nlohmann::json MakeError(const nlohmann::json &aId, const char *aCode, const std::string &aMessage);

    Core::Simulation                  &mSim;
    WsServer                          &mServer;
    VizPublisher                      &mViz;
    JsonlRecorder                     &mRecorder;
    std::filesystem::path              mOutputDir;
    Nodes::RfSimServer                *mRfSimServer = nullptr;
    std::vector<std::filesystem::path> mAllowedSpawnPaths;
    bool                               mPaused        = true;
    bool                               mQuitRequested = false;
};

} // namespace Server
} // namespace otns

#endif // OTNS_SERVER_RPC_HANDLER_HPP_
