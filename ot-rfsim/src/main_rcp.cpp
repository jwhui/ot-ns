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
#include <openthread/config.h>
#include <openthread/instance.h>
#include <openthread/ncp.h>
#include <openthread/tasklet.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <csignal>
#include <cstdio>
#include <vector>

#include "ot_rfsim_v3.h"
#include "utils/uart.h"

namespace {

volatile sig_atomic_t sTerminate = 0;

void HandleSignal(int aSignal)
{
    (void)aSignal;
    sTerminate = 1;
}

int HandleNcpSend(const uint8_t *aBuf, uint16_t aBufLength)
{
    (void)otPlatUartSend(aBuf, aBufLength);
    return static_cast<int>(aBufLength);
}

} // namespace

extern "C" {

void otPlatUartReceived(const uint8_t *aBuf, uint16_t aBufLength) { otNcpHdlcReceive(aBuf, aBufLength); }

void otPlatUartSendDone(void) { otNcpHdlcSendDone(); }

} // extern "C"

int main(int aArgc, char *aArgv[])
{
    std::signal(SIGTERM, &HandleSignal);
    std::signal(SIGINT, &HandleSignal);
    std::signal(SIGHUP, &HandleSignal);
#ifdef __linux__
    // Exit automatically if the parent otbr-agent / POSIX host exits.
    (void)::prctl(PR_SET_PDEATHSIG, SIGHUP);
#endif

    bool completedInitialSync = false;

    otRfSimSetAppName("rcp");

    while (!sTerminate)
    {
        otSysInit(aArgc, aArgv);
        if (!otRfSimIsConnected())
        {
            std::fprintf(stderr, "ot-rcp-rfsim: failed to connect to OTNS v3 simulator endpoint\n");
            return 1;
        }

        otInstance *instance = nullptr;
#if OPENTHREAD_CONFIG_MULTIPLE_INSTANCE_ENABLE
        std::vector<uint8_t> instanceBuf;
        size_t               instSize = 0;

        (void)otInstanceInit(nullptr, &instSize);
        instanceBuf.resize(instSize, 0);
        instance = otInstanceInit(instanceBuf.data(), &instSize);
#else
        instance = otInstanceInitSingle();
#endif

        if (instance == nullptr)
        {
            std::fprintf(stderr, "ot-rcp-rfsim: failed to initialize OpenThread RCP instance\n");
            otSysDeinit();
            return 1;
        }

        (void)otPlatUartEnable();
        otNcpHdlcInit(instance, &HandleNcpSend);
        otRfSimAttachInstance(instance);

        if (!completedInitialSync)
        {
            completedInitialSync = true;
            if (!otRfSimCompleteStartup(instance))
            {
                sTerminate = 1;
            }
        }

        while (!sTerminate && otRfSimIsConnected() && !otSysPseudoResetWasRequested())
        {
            otTaskletsProcess(instance);
            otSysProcessDrivers(instance);
        }

        otInstanceFinalize(instance);

        if (!otSysPseudoResetWasRequested() || !otRfSimIsConnected())
        {
            break;
        }
    }

    otSysDeinit();

    return 0;
}
