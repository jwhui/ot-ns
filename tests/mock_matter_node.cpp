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

/**
 * @file
 *   Minimal Matter-style OpenThread library-mode test executable.
 *
 *   Exercises the exact lifecycle used by upstream project-chip/connectedhomeip
 *   `all-devices-app` (`--target ...-openthread-endpoint-otns`), intentionally
 *   calling ZERO ot_rfsim_v3.h symbols:
 *     1. Strips `--thread-arg=<val>` / `--thread-arg <val>` and forwards them
 *        to `otSysInit(argc, argv)`.
 *     2. Calls `otInstanceInitSingle()` and `otAppCliInit(instance)` (linking
 *        OpenThread's real `examples/apps/cli/cli_uart.cpp`).
 *     3. Runs a `select()` event loop calling `otTaskletsProcess`,
 *        `otSysUpdateEvents`, and `otSysProcessEvents` (verifying automatic
 *        initial sync and `cli_uart` command/response bridging over `kCliData`).
 */

#include <openthread-core-config.h>
#include <openthread-select.h>
#include <openthread-system.h>
#include <openthread/cli.h>
#include <openthread/instance.h>
#include <openthread/tasklet.h>

#include <sys/select.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

extern "C" void otAppCliInit(otInstance *aInstance);

#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
// In the root OTNS CMake build, OpenThread is built with multi-instance enabled
// for in-process simulation. Bridge single-instance cli_uart.cpp symbols so this
// test still compiles and runs OpenThread's real examples/apps/cli/cli_uart.cpp.
namespace {

std::vector<uint8_t> sCliBuf;
otCliInterpreter    *sCliInterp = nullptr;

} // namespace

extern "C" void otCliInit(otInstance *aInstance, otCliOutputCallback aCallback, void *aContext)
{
    sCliBuf.assign(otCliInterpreterGetSize(), 0);
    sCliInterp = otCliInterpreterInit(sCliBuf.data(), sCliBuf.size(), aInstance, aCallback, aContext);
}

extern "C" void otCliInputLine(char *aBuf)
{
    if (sCliInterp != nullptr && aBuf != nullptr)
    {
        otCliInterpreterInputLine(sCliInterp, aBuf);
    }
}
#endif

namespace {

volatile std::sig_atomic_t sStop = 0;

void HandleSignal(int /* aSignal */) { sStop = 1; }

#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
otInstance *InitTestInstance(std::vector<uint8_t> &aStorage)
{
    size_t sz = 0;

    (void)otInstanceInit(nullptr, &sz);
    aStorage.assign(sz, 0);
    return otInstanceInit(aStorage.data(), &sz);
}
#endif

} // namespace

int main(int aArgc, char *aArgv[])
{
    std::signal(SIGTERM, HandleSignal);
    std::signal(SIGINT, HandleSignal);

    // Mirror connectedhomeip/examples/all-devices-app/posix/main.cpp:
    // extract --thread-arg values and ignore app-level Matter options.
    std::vector<std::string> threadArgs;
    static constexpr char    kPrefix[] = "--thread-arg=";
    const size_t             prefixLen = std::strlen(kPrefix);

    for (int i = 1; i < aArgc; ++i)
    {
        if (aArgv[i] == nullptr)
        {
            continue;
        }
        std::string arg = aArgv[i];

        if (arg == "--thread-arg" && i + 1 < aArgc)
        {
            threadArgs.push_back(aArgv[++i]);
        }
        else if (arg.size() > prefixLen && arg.compare(0, prefixLen, kPrefix) == 0)
        {
            threadArgs.push_back(arg.substr(prefixLen));
        }
    }

    if (threadArgs.empty())
    {
        return 2;
    }

    std::vector<char *> otArgv;

    otArgv.reserve(threadArgs.size() + 2);
    otArgv.push_back(aArgv[0]);
    for (std::string &tok : threadArgs)
    {
        otArgv.push_back(tok.data());
    }
    otArgv.push_back(nullptr);

    otSysInit(static_cast<int>(otArgv.size() - 1), otArgv.data());

#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
    std::vector<uint8_t> instBuf;
    otInstance          *instance = InitTestInstance(instBuf);
#else
    otInstance *instance = otInstanceInitSingle();
#endif

    if (instance == nullptr)
    {
        otSysDeinit();
        return 1;
    }

    // Mirrors GenericThreadStackManagerImpl_OpenThread::DoInit() when CHIP_DEVICE_CONFIG_THREAD_ENABLE_CLI=1.
    otAppCliInit(instance);

    while (!sStop)
    {
        if (otSysPseudoResetWasRequested())
        {
#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
            if (sCliInterp != nullptr)
            {
                otCliInterpreterFinalize(sCliInterp);
                sCliInterp = nullptr;
            }
#endif
            otInstanceFinalize(instance);
            otSysInit(static_cast<int>(otArgv.size() - 1), otArgv.data());
#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
            instance = InitTestInstance(instBuf);
#else
            instance = otInstanceInitSingle();
#endif
            if (instance == nullptr)
            {
                break;
            }
            otAppCliInit(instance);
        }

        fd_set         readfds;
        fd_set         writefds;
        fd_set         errorfds;
        int            maxfd   = -1;
        struct timeval timeout = {1, 0};

        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_ZERO(&errorfds);

        otTaskletsProcess(instance);
        otSysUpdateEvents(instance, &maxfd, &readfds, &writefds, &errorfds, &timeout);

        if (maxfd < 0)
        {
            // Simulator closed the ot-rfsim v3 socket (kShutdown / EOF).
            break;
        }

        int rval = ::select(maxfd + 1, &readfds, &writefds, &errorfds, &timeout);

        if (rval < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        otSysProcessEvents(instance, &readfds, &writefds, &errorfds);
    }

#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
    if (sCliInterp != nullptr)
    {
        otCliInterpreterFinalize(sCliInterp);
        sCliInterp = nullptr;
    }
#endif
    if (instance != nullptr)
    {
        otInstanceFinalize(instance);
    }
    otSysDeinit();

    return 0;
}
