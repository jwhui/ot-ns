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

#include <openthread/tasklet.h>
#include <openthread/platform/alarm-micro.h>
#include <openthread/platform/alarm-milli.h>
#include <openthread/platform/entropy.h>
#include <openthread/platform/logging.h>
#include <openthread/platform/memory.h>
#include <openthread/platform/misc.h>
#include <openthread/platform/otns.h>
#include <openthread/platform/radio.h>
#include <openthread/platform/settings.h>
#include <openthread/platform/time.h>

#include <cstdlib>
#include <cstring>

#include "core/simulation.hpp"
#include "nodes/inproc_node.hpp"

using otns::Core::Simulation;
using otns::Nodes::InProcNode;

namespace {

inline InProcNode *GetNode(otInstance *aInstance) { return InProcNode::FromInstance(aInstance); }

} // namespace

extern "C" {

//---------------------------------------------------------------------------------------------------------------------
// Heap & Misc

void *otPlatCAlloc(size_t aNum, size_t aSize) { return std::calloc(aNum, aSize); }

void otPlatFree(void *aPtr) { std::free(aPtr); }

void otPlatReset(otInstance *aInstance) { (void)aInstance; }

otPlatResetReason otPlatGetResetReason(otInstance *aInstance)
{
    (void)aInstance;
    return OT_PLAT_RESET_REASON_POWER_ON;
}

void otPlatWakeHost(void) {}

void otTaskletsSignalPending(otInstance *aInstance)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SignalPendingTasklets();
    }
}

otError otPlatEntropyGet(uint8_t *aOutput, uint16_t aOutputLength)
{
    Simulation *sim = Simulation::GetCurrent();

    if (aOutput == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    if (sim == nullptr)
    {
        std::memset(aOutput, 0x42, aOutputLength);
        return OT_ERROR_NONE;
    }

    auto &rng = sim->GetPlatformPrng();

    for (uint16_t i = 0; i < aOutputLength; ++i)
    {
        aOutput[i] = static_cast<uint8_t>(rng.NextU32() & 0xFFu);
    }

    return OT_ERROR_NONE;
}

void otPlatLogOutput(otInstance *aInstance, otLogLevel aLogLevel, const char *aLogLine)
{
    if (auto *node = GetNode(aInstance))
    {
        node->AppendLog(aLogLevel, aLogLine);
    }
}

void otPlatOtnsStatus(const char *aStatus) { (void)aStatus; }

//---------------------------------------------------------------------------------------------------------------------
// Alarms & Time

uint64_t otPlatTimeGet(void)
{
    Simulation *sim = Simulation::GetCurrent();

    return (sim != nullptr) ? sim->GetNowUs() : 0;
}

uint16_t otPlatTimeGetXtalAccuracy(void) { return 0; }

uint32_t otPlatAlarmMilliGetNow(void) { return static_cast<uint32_t>(otPlatTimeGet() / 1000ULL); }

void otPlatAlarmMilliStartAt(otInstance *aInstance, uint32_t aT0, uint32_t aDt)
{
    if (auto *node = GetNode(aInstance))
    {
        node->StartAlarmMilliAt(aT0, aDt);
    }
}

void otPlatAlarmMilliStop(otInstance *aInstance)
{
    if (auto *node = GetNode(aInstance))
    {
        node->StopAlarmMilli();
    }
}

uint32_t otPlatAlarmMicroGetNow(void) { return static_cast<uint32_t>(otPlatTimeGet()); }

void otPlatAlarmMicroStartAt(otInstance *aInstance, uint32_t aT0, uint32_t aDt)
{
    if (auto *node = GetNode(aInstance))
    {
        node->StartAlarmMicroAt(aT0, aDt);
    }
}

void otPlatAlarmMicroStop(otInstance *aInstance)
{
    if (auto *node = GetNode(aInstance))
    {
        node->StopAlarmMicro();
    }
}

//---------------------------------------------------------------------------------------------------------------------
// Settings

void otPlatSettingsInit(otInstance *aInstance, const uint16_t *aSensitiveKeys, uint16_t aSensitiveKeysLength)
{
    (void)aInstance;
    (void)aSensitiveKeys;
    (void)aSensitiveKeysLength;
}

void otPlatSettingsDeinit(otInstance *aInstance) { (void)aInstance; }

otError otPlatSettingsGet(otInstance *aInstance, uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLength)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->SettingsGet(aKey, aIndex, aValue, aValueLength) : OT_ERROR_NOT_FOUND;
}

otError otPlatSettingsSet(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->SettingsSet(aKey, aValue, aValueLength) : OT_ERROR_FAILED;
}

otError otPlatSettingsAdd(otInstance *aInstance, uint16_t aKey, const uint8_t *aValue, uint16_t aValueLength)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->SettingsAdd(aKey, aValue, aValueLength) : OT_ERROR_FAILED;
}

otError otPlatSettingsDelete(otInstance *aInstance, uint16_t aKey, int aIndex)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->SettingsDelete(aKey, aIndex) : OT_ERROR_NOT_FOUND;
}

void otPlatSettingsWipe(otInstance *aInstance)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SettingsWipe();
    }
}

//---------------------------------------------------------------------------------------------------------------------
// Radio

otRadioCaps otPlatRadioGetCaps(otInstance *aInstance)
{
    (void)aInstance;
    return OT_RADIO_CAPS_ACK_TIMEOUT | OT_RADIO_CAPS_TRANSMIT_SEC | OT_RADIO_CAPS_ENERGY_SCAN |
           OT_RADIO_CAPS_ALT_SHORT_ADDR;
}

int8_t otPlatRadioGetReceiveSensitivity(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->GetRxSensitivity() : -100;
}

void otPlatRadioGetIeeeEui64(otInstance *aInstance, uint8_t *aIeeeEui64)
{
    std::memset(aIeeeEui64, 0, 8);
    if (auto *node = GetNode(aInstance))
    {
        uint32_t id = node->GetId();

        aIeeeEui64[0] = 0x02; // Locally administered unicast OUI
        aIeeeEui64[4] = static_cast<uint8_t>((id >> 24u) & 0xFFu);
        aIeeeEui64[5] = static_cast<uint8_t>((id >> 16u) & 0xFFu);
        aIeeeEui64[6] = static_cast<uint8_t>((id >> 8u) & 0xFFu);
        aIeeeEui64[7] = static_cast<uint8_t>(id & 0xFFu);
    }
}

void otPlatRadioSetPanId(otInstance *aInstance, otPanId aPanId)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetPanId(aPanId);
    }
}

void otPlatRadioSetExtendedAddress(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetExtendedAddress(aExtAddress);
    }
}

void otPlatRadioSetShortAddress(otInstance *aInstance, otShortAddress aShortAddress)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetShortAddress(aShortAddress);
    }
}

void otPlatRadioSetAlternateShortAddress(otInstance *aInstance, otShortAddress aShortAddress)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetAltShortAddress(aShortAddress);
    }
}

bool otPlatRadioGetPromiscuous(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->GetPromiscuous() : false;
}

void otPlatRadioSetPromiscuous(otInstance *aInstance, bool aEnable)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetPromiscuous(aEnable);
    }
}

otRadioState otPlatRadioGetState(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioGetState() : OT_RADIO_STATE_DISABLED;
}

otError otPlatRadioEnable(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioEnable() : OT_ERROR_FAILED;
}

otError otPlatRadioDisable(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioDisable() : OT_ERROR_FAILED;
}

bool otPlatRadioIsEnabled(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) && (node->RadioGetState() != OT_RADIO_STATE_DISABLED);
}

otError otPlatRadioSleep(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioSleep() : OT_ERROR_INVALID_STATE;
}

otError otPlatRadioReceive(otInstance *aInstance, uint8_t aChannel)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioReceive(aChannel) : OT_ERROR_INVALID_STATE;
}

otRadioFrame *otPlatRadioGetTransmitBuffer(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioGetTxBuffer() : nullptr;
}

otError otPlatRadioTransmit(otInstance *aInstance, otRadioFrame *aFrame)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioTransmit(aFrame) : OT_ERROR_INVALID_STATE;
}

int8_t otPlatRadioGetRssi(otInstance *aInstance)
{
    auto *node = GetNode(aInstance);

    if (node == nullptr)
    {
        return OT_RADIO_RSSI_INVALID;
    }

    const auto *rn = node->GetSim().GetRadioModel().GetNode(node->GetId());

    if (rn == nullptr)
    {
        return OT_RADIO_RSSI_INVALID;
    }

    return otns::Rf::ClipRssiToInt8(node->GetSim().GetRadioModel().SampleChannelRssi(node->GetId(), rn->mChannel));
}

otError otPlatRadioEnergyScan(otInstance *aInstance, uint8_t aScanChannel, uint16_t aScanDuration)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->RadioEnergyScan(aScanChannel, aScanDuration) : OT_ERROR_INVALID_STATE;
}

void otPlatRadioEnableSrcMatch(otInstance *aInstance, bool aEnable)
{
    if (auto *node = GetNode(aInstance))
    {
        node->EnableSrcMatch(aEnable);
    }
}

otError otPlatRadioAddSrcMatchShortEntry(otInstance *aInstance, otShortAddress aShortAddress)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->AddSrcMatchShort(aShortAddress) : OT_ERROR_FAILED;
}

otError otPlatRadioAddSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->AddSrcMatchExt(aExtAddress) : OT_ERROR_FAILED;
}

otError otPlatRadioClearSrcMatchShortEntry(otInstance *aInstance, otShortAddress aShortAddress)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->ClearSrcMatchShort(aShortAddress) : OT_ERROR_NO_ADDRESS;
}

otError otPlatRadioClearSrcMatchExtEntry(otInstance *aInstance, const otExtAddress *aExtAddress)
{
    auto *node = GetNode(aInstance);

    return (node != nullptr) ? node->ClearSrcMatchExt(aExtAddress) : OT_ERROR_NO_ADDRESS;
}

void otPlatRadioClearSrcMatchShortEntries(otInstance *aInstance)
{
    if (auto *node = GetNode(aInstance))
    {
        node->ClearAllSrcMatchShort();
    }
}

void otPlatRadioClearSrcMatchExtEntries(otInstance *aInstance)
{
    if (auto *node = GetNode(aInstance))
    {
        node->ClearAllSrcMatchExt();
    }
}

void otPlatRadioSetMacKey(otInstance             *aInstance,
                          uint8_t                 aKeyIdMode,
                          uint8_t                 aKeyIndex,
                          const otMacKeyMaterial *aPrevKey,
                          const otMacKeyMaterial *aCurrKey,
                          const otMacKeyMaterial *aNextKey,
                          otRadioKeyType          aKeyType)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetMacKey(aKeyIdMode, aKeyIndex, aPrevKey, aCurrKey, aNextKey, aKeyType);
    }
}

void otPlatRadioSetMacFrameCounter(otInstance *aInstance, uint32_t aMacFrameCounter)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetMacFrameCounter(aMacFrameCounter);
    }
}

void otPlatRadioSetMacFrameCounterIfLarger(otInstance *aInstance, uint32_t aMacFrameCounter)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetMacFrameCounterIfLarger(aMacFrameCounter);
    }
}

uint64_t otPlatRadioGetNow(otInstance *aInstance)
{
    (void)aInstance;
    return otPlatTimeGet();
}

otError otPlatRadioGetTransmitPower(otInstance *aInstance, int8_t *aPower)
{
    auto *node = GetNode(aInstance);

    if (node == nullptr || aPower == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    *aPower = node->GetTxPower();

    return OT_ERROR_NONE;
}

otError otPlatRadioSetTransmitPower(otInstance *aInstance, int8_t aPower)
{
    auto *node = GetNode(aInstance);

    if (node == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    node->SetTxPower(aPower);

    return OT_ERROR_NONE;
}

otError otPlatRadioGetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t *aThreshold)
{
    auto *node = GetNode(aInstance);

    if (node == nullptr || aThreshold == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    *aThreshold = node->GetCcaThreshold();

    return OT_ERROR_NONE;
}

otError otPlatRadioSetCcaEnergyDetectThreshold(otInstance *aInstance, int8_t aThreshold)
{
    auto *node = GetNode(aInstance);

    if (node == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    node->SetCcaThreshold(aThreshold);

    return OT_ERROR_NONE;
}

otError otPlatRadioEnableCsl(otInstance         *aInstance,
                             uint32_t            aCslPeriod,
                             otShortAddress      aShortAddr,
                             const otExtAddress *aExtAddr)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetCslParams(aCslPeriod, aShortAddr, aExtAddr);
        return OT_ERROR_NONE;
    }

    return OT_ERROR_FAILED;
}

otError otPlatRadioResetCsl(otInstance *aInstance)
{
    if (auto *node = GetNode(aInstance))
    {
        node->SetCslParams(0, OT_RADIO_INVALID_SHORT_ADDR, nullptr);
        return OT_ERROR_NONE;
    }

    return OT_ERROR_FAILED;
}

void otPlatRadioUpdateCslSampleTime(otInstance *aInstance, uint32_t aCslSampleTime)
{
    if (auto *node = GetNode(aInstance))
    {
        node->UpdateCslSampleTime(aCslSampleTime);
    }
}

uint8_t otPlatRadioGetCslAccuracy(otInstance *aInstance)
{
    (void)aInstance;
    return 0;
}

uint8_t otPlatRadioGetCslUncertainty(otInstance *aInstance)
{
    (void)aInstance;
    return 0;
}

otError otPlatRadioConfigureEnhAckProbing(otInstance          *aInstance,
                                          otLinkMetrics        aLinkMetrics,
                                          const otShortAddress aShortAddress,
                                          const otExtAddress  *aExtAddress)
{
    (void)aInstance;
    (void)aLinkMetrics;
    (void)aShortAddress;
    (void)aExtAddress;
    return OT_ERROR_NOT_IMPLEMENTED;
}

uint32_t otPlatRadioGetSupportedChannelMask(otInstance *aInstance)
{
    (void)aInstance;
    return OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MASK;
}

uint32_t otPlatRadioGetPreferredChannelMask(otInstance *aInstance)
{
    (void)aInstance;
    return OT_RADIO_2P4GHZ_OQPSK_CHANNEL_MASK;
}

} // extern "C"
