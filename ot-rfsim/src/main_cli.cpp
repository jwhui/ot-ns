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

#include <openthread-core-config.h>
#include <openthread-system.h>
#include <openthread/cli.h>
#include <openthread/config.h>
#include <openthread/instance.h>

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "ot_rfsim_v3.h"

namespace {

volatile sig_atomic_t sTerminate = 0;

void HandleSignal(int aSignal)
{
    (void)aSignal;
    sTerminate = 1;
}

int HandleCliOutput(void *aContext, const char *aFormat, va_list aArgs)
{
    char buf[1024];
    int  n = std::vsnprintf(buf, sizeof(buf), aFormat, aArgs);

    (void)aContext;
    if (n > 0)
    {
        size_t len = std::min(static_cast<size_t>(n), sizeof(buf) - 1);

        otRfSimSendCliOutput(buf, len);
    }

    return n;
}

void HandleCliInput(const char *aLine, void *aContext)
{
    if (aLine == nullptr)
    {
        return;
    }

    std::string mutableLine(aLine);

#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
    auto *interp = static_cast<otCliInterpreter *>(aContext);

    if (interp != nullptr)
    {
        otCliInterpreterInputLine(interp, mutableLine.data());
    }
#else
    (void)aContext;
    otCliInputLine(mutableLine.data());
#endif
}

} // namespace

int main(int aArgc, char *aArgv[])
{
    std::signal(SIGTERM, &HandleSignal);
    std::signal(SIGINT, &HandleSignal);

    otSysInit(aArgc, aArgv);
    if (!otRfSimIsConnected())
    {
        std::fprintf(stderr, "ot-cli-ftd-rfsim: failed to connect to OTNS v3 simulator endpoint\n");
        return 1;
    }

    otInstance *instance = nullptr;

#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
    std::vector<uint8_t> instanceBuf;
    std::vector<uint8_t> cliBuf;
    otCliInterpreter    *cliInterp = nullptr;
    size_t               instSize  = 0;

    (void)otInstanceInit(nullptr, &instSize);
    instanceBuf.resize(instSize, 0);
    instance = otInstanceInit(instanceBuf.data(), &instSize);

    size_t cliSize = otCliInterpreterGetSize();

    cliBuf.resize(cliSize, 0);
    cliInterp = otCliInterpreterInit(cliBuf.data(), cliBuf.size(), instance, &HandleCliOutput, nullptr);
#if OPENTHREAD_CONFIG_CLI_PROMPT_ENABLE
    otCliInterpreterSetPromptConfig(cliInterp, false);
#endif
    otRfSimSetCliHandler(&HandleCliInput, cliInterp);
#else
    instance = otInstanceInitSingle();
    otCliInit(instance, &HandleCliOutput, nullptr);
    otRfSimSetCliHandler(&HandleCliInput, nullptr);
#endif

    otRfSimAttachInstance(instance);

    if (!otRfSimCompleteStartup(instance))
    {
        sTerminate = 1;
    }

    while (!sTerminate && otRfSimIsConnected())
    {
        otSysProcessDrivers(instance);
    }

#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
    if (cliInterp != nullptr)
    {
        otCliInterpreterFinalize(cliInterp);
    }
#endif
    if (instance != nullptr)
    {
        otInstanceFinalize(instance);
    }
    otSysDeinit();

    return 0;
}
