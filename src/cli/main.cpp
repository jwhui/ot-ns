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

#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "cli/repl.hpp"
#include "core/simulation.hpp"
#include "core/topology.hpp"
#include "nodes/rfsim_server.hpp"
#include "server/replay.hpp"
#include "server/rpc_handler.hpp"
#include "server/viz_publisher.hpp"
#include "server/ws_server.hpp"

namespace {

volatile sig_atomic_t sStopRequested = 0;

void HandleSignal(int aSignal)
{
    (void)aSignal;
    sStopRequested = 1;
}

void PrintUsage(const char *aProg)
{
    std::cout << "Usage: " << aProg << " [options]\n"
              << "  --port <u16>          HTTP/WebSocket port (default: 8999, 0 = ephemeral)\n"
              << "  --bind <ip>           Bind address (default: 127.0.0.1)\n"
              << "  --rpc-socket <path>   Optional Unix domain socket for JSON-RPC\n"
              << "  --rfsim-socket <path> Optional Unix socket for external ot-rfsim v3 nodes\n"
              << "  --rfsim-tcp <port>    Optional TCP loopback port for external ot-rfsim v3 nodes\n"
              << "  --output-dir <dir>    Contained directory for PCAP/KPI/topology/replay outputs\n"
              << "  --web-dir <dir>       Serve web UI assets from directory instead of embedded bundle\n"
              << "  --topology <file>     Load JSON topology file at startup\n"
              << "  --replay <file>       Stream a .jsonl replay file to connected Web UI clients\n"
              << "  --seed <u64>          Deterministic PRNG seed (default: 1)\n"
              << "  --rf-model <preset>   Ideal|IdealRssi|MutualInterference|Midisc|Outdoor\n"
              << "  --speed <float>       Simulation speed (0 = max, 1 = real-time; default: 1.0)\n"
              << "  --headless            Disable interactive stdin REPL\n"
              << "  --help                Show this help\n";
}

bool ParsePresetArg(const std::string &aStr, otns::Rf::RadioModelPreset &aOut)
{
    if (aStr == "Ideal")
    {
        aOut = otns::Rf::RadioModelPreset::kIdeal;
        return true;
    }
    if (aStr == "IdealRssi")
    {
        aOut = otns::Rf::RadioModelPreset::kIdealRssi;
        return true;
    }
    if (aStr == "MutualInterference" || aStr == "MI")
    {
        aOut = otns::Rf::RadioModelPreset::kMutualInterference;
        return true;
    }
    if (aStr == "Midisc")
    {
        aOut = otns::Rf::RadioModelPreset::kMidisc;
        return true;
    }
    if (aStr == "Outdoor")
    {
        aOut = otns::Rf::RadioModelPreset::kOutdoor;
        return true;
    }
    return false;
}

} // namespace

int main(int aArgc, char *aArgv[])
{
    std::signal(SIGINT, &HandleSignal);
    std::signal(SIGTERM, &HandleSignal);

    uint16_t                     httpPort = 8999;
    std::string                  bindAddr = "127.0.0.1";
    std::string                  rpcSocket;
    std::string                  rfsimSocket;
    int                          rfsimTcpPort = -1;
    std::filesystem::path        outputDir    = std::filesystem::current_path() / "otns_out";
    std::filesystem::path        webDir;
    std::string                  topologyPath;
    std::string                  replayPath;
    bool                         headless = false;
    otns::Core::SimulationConfig simCfg;

    simCfg.mSpeed = 1.0;

    for (int i = 1; i < aArgc; ++i)
    {
        std::string arg = aArgv[i];

        if (arg == "--help" || arg == "-h")
        {
            PrintUsage(aArgv[0]);
            return 0;
        }
        if (arg == "--headless")
        {
            headless = true;
            continue;
        }
        if (i + 1 >= aArgc)
        {
            std::cerr << "Missing value for option: " << arg << "\n";
            return 2;
        }

        std::string val = aArgv[++i];

        if (arg == "--port")
        {
            httpPort = static_cast<uint16_t>(std::strtoul(val.c_str(), nullptr, 10));
        }
        else if (arg == "--bind")
        {
            bindAddr = val;
        }
        else if (arg == "--rpc-socket")
        {
            rpcSocket = val;
        }
        else if (arg == "--rfsim-socket")
        {
            rfsimSocket = val;
        }
        else if (arg == "--rfsim-tcp")
        {
            rfsimTcpPort = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
        }
        else if (arg == "--output-dir")
        {
            outputDir = val;
        }
        else if (arg == "--web-dir")
        {
            webDir = val;
        }
        else if (arg == "--topology")
        {
            topologyPath = val;
        }
        else if (arg == "--replay")
        {
            replayPath = val;
        }
        else if (arg == "--seed")
        {
            simCfg.mSeed = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
        }
        else if (arg == "--rf-model")
        {
            if (!ParsePresetArg(val, simCfg.mRfPreset))
            {
                std::cerr << "Unknown --rf-model preset: " << val << "\n";
                return 2;
            }
        }
        else if (arg == "--speed")
        {
            simCfg.mSpeed = std::strtod(val.c_str(), nullptr);
        }
        else
        {
            std::cerr << "Unknown option: " << arg << "\n";
            return 2;
        }
    }

    std::error_code ec;

    std::filesystem::create_directories(outputDir, ec);
    if (ec)
    {
        std::cerr << "Failed to create output directory: " << outputDir.string() << "\n";
        return 1;
    }

    otns::Core::Simulation      sim(simCfg);
    otns::Server::WsServer      wsServer;
    otns::Server::JsonlRecorder recorder;

    if (!webDir.empty())
    {
        wsServer.SetWebDir(webDir);
    }

    if (!wsServer.ListenHttp(httpPort, bindAddr))
    {
        std::cerr << "Failed to listen HTTP/WS on " << bindAddr << ":" << httpPort << "\n";
        return 1;
    }

    if (!rpcSocket.empty() && !wsServer.ListenRpcUnix(rpcSocket))
    {
        std::cerr << "Failed to listen Unix JSON-RPC on " << rpcSocket << "\n";
        return 1;
    }

    // Replay-only streaming mode if --replay was supplied.
    if (!replayPath.empty())
    {
        otns::Server::JsonlReplayer replayer;
        std::string                 err;

        if (!replayer.LoadDirect(replayPath, err))
        {
            std::cerr << "Failed to load replay file: " << err << "\n";
            return 1;
        }

        std::cout << "OTNS_HTTP_PORT=" << wsServer.GetHttpPort() << "\n";
        std::cout << "OTNS v3 replay server on http://" << bindAddr << ":" << wsServer.GetHttpPort() << " ("
                  << replayer.GetEventCount() << " events)\n"
                  << std::flush;

        wsServer.SetOnConnect([&](otns::Server::ClientId aClientId) {
            replayer.Reset();
            for (const auto &line : replayer.PopDueEvents(0))
            {
                (void)wsServer.SendText(aClientId, line);
            }
        });

        auto   wallStart = std::chrono::steady_clock::now();
        double playSpeed = (simCfg.mSpeed > 0.0) ? simCfg.mSpeed : 1.0;

        while (!sStopRequested)
        {
            wsServer.PollOnce(20);
            if (!replayer.IsFinished() && wsServer.GetClientCount() > 0)
            {
                auto wallUs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wallStart)
                        .count());
                uint64_t simTs = static_cast<uint64_t>(static_cast<double>(wallUs) * playSpeed);

                for (const auto &line : replayer.PopDueEvents(simTs))
                {
                    wsServer.BroadcastText(line);
                }
            }
        }
        return 0;
    }

    otns::Nodes::RfSimServer rfsimServer(sim);

    if (!rfsimSocket.empty())
    {
        if (!rfsimServer.ListenUnix(rfsimSocket))
        {
            std::cerr << "Failed to listen ot-rfsim Unix socket on " << rfsimSocket << "\n";
            return 1;
        }
    }
    else if (rfsimTcpPort >= 0)
    {
        if (!rfsimServer.ListenTcp(static_cast<uint16_t>(rfsimTcpPort), "127.0.0.1"))
        {
            std::cerr << "Failed to listen ot-rfsim TCP port " << rfsimTcpPort << "\n";
            return 1;
        }
    }

    otns::Server::VizPublisher viz(sim, wsServer, &recorder);
    otns::Server::RpcHandler   rpc(sim, wsServer, viz, recorder, outputDir);
    otns::Cli::Repl            repl(sim, viz, rpc, recorder, outputDir);

    sim.SetPacketObserver([&viz](const otns::Core::PacketObservation &aObs) { viz.OnPacket(aObs); });
    wsServer.SetOnConnect([&viz, &rpc](otns::Server::ClientId aId) { viz.OnClientConnect(aId, rpc.IsPaused()); });
    wsServer.SetOnDisconnect([&viz](otns::Server::ClientId aId) { viz.OnClientDisconnect(aId); });
    wsServer.SetOnMessage(
        [&rpc](otns::Server::ClientId aId, const std::string &aMsg) { rpc.HandleMessage(aId, aMsg); });

    if (!topologyPath.empty())
    {
        otns::Core::TopologySpec spec;
        std::string              err;

        if (!otns::Core::LoadTopologyFileContained(std::filesystem::path(topologyPath).parent_path(),
                                                   std::filesystem::path(topologyPath).filename().string(), spec, err))
        {
            std::cerr << "Failed to load topology: " << err << "\n";
            return 1;
        }
        (void)sim.LoadTopology(spec);
    }

    std::cout << "OTNS_HTTP_PORT=" << wsServer.GetHttpPort() << "\n";
    if (!rpcSocket.empty())
    {
        std::cout << "OTNS_RPC_SOCKET=" << rpcSocket << "\n";
    }
    std::cout << "OTNS v3 listening on http://" << bindAddr << ":" << wsServer.GetHttpPort() << "\n" << std::flush;

    bool        useRepl = !headless && (::isatty(STDIN_FILENO) != 0);
    int         replFd  = useRepl ? STDIN_FILENO : -1;
    std::string stdinBuf;

    if (useRepl)
    {
        std::cout << "> " << std::flush;
    }

    auto lastStepWall = std::chrono::steady_clock::now();

    while (!sStopRequested && !rpc.IsQuitRequested())
    {
        bool stdinReady = false;
        int  pollMs     = rpc.IsPaused() ? 25 : 5;

        wsServer.PollOnce(pollMs, replFd, &stdinReady);

        if (stdinReady && replFd >= 0)
        {
            char    tmp[1024];
            ssize_t n = ::read(replFd, tmp, sizeof(tmp));

            if (n <= 0)
            {
                replFd = -1;
            }
            else
            {
                stdinBuf.append(tmp, static_cast<size_t>(n));
                size_t nl = 0;

                while ((nl = stdinBuf.find('\n')) != std::string::npos)
                {
                    std::string line = stdinBuf.substr(0, nl);

                    stdinBuf.erase(0, nl + 1);
                    if (!repl.ExecuteLine(line, std::cout))
                    {
                        break;
                    }
                    std::cout << "> " << std::flush;
                }
            }
        }

        auto now = std::chrono::steady_clock::now();

        if (!rpc.IsPaused())
        {
            double wallDeltaUs =
                static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(now - lastStepWall).count());
            double   spd     = sim.GetSpeed();
            uint64_t sliceUs = (spd <= 0.0) ? 50000ULL : static_cast<uint64_t>(std::max(1000.0, wallDeltaUs * spd));

            rpc.StepContinuous(sliceUs);
        }
        lastStepWall = now;
    }

    recorder.Close();
    wsServer.Close();
    rfsimServer.Close();

    return 0;
}
