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

#ifndef OTNS_OT_RFSIM_CLIENT_HPP_
#define OTNS_OT_RFSIM_CLIENT_HPP_

#include <openthread/instance.h>
#include <openthread/platform/infra_if.h>
#include <openthread/platform/logging.h>
#include <openthread/platform/radio.h>
#include <openthread/platform/trel.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "mac_frame.h"
#include "ot_rfsim_v3.h"
#include "protocol/rfsim_v3_codec.hpp"

namespace otns {
namespace RfSim {

/**
 * Single-node ot-rfsim v3 client engine implementing OpenThread's platform PAL
 * over a Unix or TCP stream socket in lock-step virtual time.
 */
class Client
{
public:
    static Client &Get(void);

    void SetAppName(const std::string &aName);
    void Configure(const std::string &aEndpoint, uint32_t aNodeId, uint64_t aSeed);
    bool Connect(void);
    void Disconnect(void);
    bool IsConnected(void) const { return mSockFd >= 0 && !mDisconnected; }
    int  GetSocketFd(void) const { return mSockFd; }

    uint32_t GetNodeId(void) const { return mNodeId; }
    uint64_t GetNowUs(void) const { return mNowUs; }
    uint8_t  NextRandomByte(void);

    void AttachInstance(otInstance *aInstance) { mInstance = aInstance; }
    void SetCliHandler(otRfSimCliInputCallback aCallback, void *aContext);
    void SendCliOutput(const char *aText, size_t aLength);
    bool CompleteStartup(otInstance *aInstance);

    void ProcessDrivers(otInstance *aInstance);
    void ProcessReadySocket(otInstance *aInstance);
    void ProcessPendingWork(otInstance *aInstance);
    bool HasPendingWork(otInstance *aInstance) const;
    void ResetForPseudoReset(void);

    // Virtual backbone hooks (TREL / Infra-If AIL / Host UDP)
    bool GetInfraIfLinkLocalAddress(uint8_t aAddr[16]) const;
    bool GetBackboneAddress(uint8_t aAddr[16]) const;

    void SetTrelPacketHandler(otRfSimPacketCallback aCallback, void *aContext);
    void SetInfraIfPacketHandler(otRfSimPacketCallback aCallback, void *aContext);
    void SetHostUdpPacketHandler(otRfSimPacketCallback aCallback, void *aContext);

    void TrelEnable(uint16_t *aUdpPort);
    void TrelDisable(void);
    bool TrelRegisterService(uint16_t aPort, const uint8_t *aTxtData, uint16_t aTxtLength);
    bool TrelSend(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength);
    const otPlatTrelCounters *TrelGetCounters(void) const { return &mTrelCounters; }
    void                      TrelResetCounters(void);

    bool    InfraIfInit(otInstance *aInstance, uint32_t aInfraIfIndex);
    void    InfraIfDeinit(otInstance *aInstance);
    bool    InfraIfHasAddress(uint32_t aInfraIfIndex, const otIp6Address *aAddress);
    otError InfraIfSendIcmp6Nd(uint32_t            aInfraIfIndex,
                               const otIp6Address *aDestAddress,
                               const uint8_t      *aBuffer,
                               uint16_t            aBufferLength);
    bool    InfraIfSendPacket(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength);
    otError InfraIfGetLinkLayerAddress(uint32_t aInfraIfIndex, otPlatInfraIfLinkLayerAddress *aAddr);
    void    InfraIfDhcp6PdSetListening(bool aEnable, uint32_t aInfraIfIndex);
    void    InfraIfDhcp6PdSend(otMessage *aMessage, const otIp6Address *aDestAddress, uint32_t aInfraIfIndex);

    bool HostUdpBind(uint16_t aPort);
    bool HostUdpUnbind(uint16_t aPort);
    bool HostUdpSend(uint16_t       aSrcPort,
                     const uint8_t  aDstIp6[16],
                     uint16_t       aDstPort,
                     const uint8_t *aPayload,
                     uint16_t       aLength);

    // Spinel UART platform hooks (used by ot-rcp-rfsim)
    otError UartEnable(void);
    otError UartDisable(void);
    otError UartSend(const uint8_t *aBuf, uint16_t aBufLength);
    otError UartFlush(void);
    bool    IsUartEnabled(void) const { return mUartEnabled; }
    bool    HasCompletedStartup(void) const { return mCompletedStartup; }
    void    EnsureStartupCompleted(otInstance *aInstance);

    // Alarm platform hooks
    void StartAlarmMilliAt(uint32_t aT0Ms, uint32_t aDtMs);
    void StopAlarmMilli(void);
    void StartAlarmMicroAt(uint32_t aT0Us, uint32_t aDtUs);
    void StopAlarmMicro(void);

    // Radio platform hooks
    otRadioState  RadioGetState(void) const { return mRadioState; }
    otError       RadioEnable(void);
    otError       RadioDisable(void);
    otError       RadioSleep(void);
    otError       RadioReceive(uint8_t aChannel);
    otRadioFrame *RadioGetTxBuffer(void) { return &mTxFrame; }
    otError       RadioTransmit(otRadioFrame *aFrame);
    otError       RadioEnergyScan(uint8_t aChannel, uint16_t aDurationMs);

    void SetPanId(otPanId aPanId) { mPanId = aPanId; }
    void SetShortAddress(otShortAddress aAddr) { mRadioCtx.mShortAddress = aAddr; }
    void SetAltShortAddress(otShortAddress aAddr) { mRadioCtx.mAlternateShortAddress = aAddr; }
    void SetExtendedAddress(const otExtAddress *aExtAddr);
    bool GetPromiscuous(void) const { return mPromiscuous; }
    void SetPromiscuous(bool aEnable) { mPromiscuous = aEnable; }

    int8_t GetTxPower(void) const { return mTxPowerDbm; }
    void   SetTxPower(int8_t aPowerDbm);
    int8_t GetRxSensitivity(void) const { return mRxSensitivityDbm; }
    int8_t GetCcaThreshold(void) const { return mCcaThresholdDbm; }
    void   SetCcaThreshold(int8_t aThresholdDbm) { mCcaThresholdDbm = aThresholdDbm; }
    int8_t GetLastRssi(void) const { return mLastRxRssiDbm; }

    void    EnableSrcMatch(bool aEnable) { mSrcMatchEnabled = aEnable; }
    otError AddSrcMatchShort(otShortAddress aAddr);
    otError AddSrcMatchExt(const otExtAddress *aAddr);
    otError ClearSrcMatchShort(otShortAddress aAddr);
    otError ClearSrcMatchExt(const otExtAddress *aAddr);
    void    ClearAllSrcMatchShort(void) { mSrcMatchShort.clear(); }
    void    ClearAllSrcMatchExt(void) { mSrcMatchExt.clear(); }

    void SetMacKey(uint8_t                 aKeyIdMode,
                   uint8_t                 aKeyIndex,
                   const otMacKeyMaterial *aPrevKey,
                   const otMacKeyMaterial *aCurrKey,
                   const otMacKeyMaterial *aNextKey,
                   otRadioKeyType          aKeyType);
    void SetMacFrameCounter(uint32_t aCounter);
    void SetMacFrameCounterIfLarger(uint32_t aCounter);

    void SetCslParams(uint32_t aPeriod, otShortAddress aShortAddr, const otExtAddress *aExtAddr);
    void UpdateCslSampleTime(uint32_t aSampleTimeUs) { mRadioCtx.mCslSampleTime = aSampleTimeUs; }

    // Settings platform hooks
    otError SettingsGet(uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLen) const;
    otError SettingsSet(uint16_t aKey, const uint8_t *aValue, uint16_t aValueLen);
    otError SettingsAdd(uint16_t aKey, const uint8_t *aValue, uint16_t aValueLen);
    otError SettingsDelete(uint16_t aKey, int aIndex);
    void    SettingsWipe(void) { mSettings.clear(); }

    void SendLog(otLogLevel aLevel, const char *aLine);
    void SignalPendingTasklets(void) { mHasPendingTasklets = true; }

    bool PseudoResetRequested(void) const { return mPseudoResetRequested; }
    void RequestPseudoReset(void) { mPseudoResetRequested = true; }
    void ClearPseudoReset(void) { mPseudoResetRequested = false; }

private:
    Client(void);

    bool SendFrame(const Protocol::Frame &aFrame);
    bool RecvMoreBytes(int aTimeoutMs);
    void DrainFrames(otInstance *aInstance);
    void HandleFrame(otInstance *aInstance, const Protocol::Frame &aFrame);
    void DrainTasklets(otInstance *aInstance);
    void SyncAlarmToSim(void);
    void SyncStatusToSim(otInstance *aInstance, bool aForce = false);
    void SendRadioState(void);
    void SendStepDone(const std::vector<uint8_t> *aAckPsdu = nullptr);
    bool HasFramePendingFor(const otMacAddress &aSrcAddr) const;
    void FlushUartNonBlocking(void);
    void EnsureInfraIfJoined(uint32_t aInfraIfIndex);

    std::string mAppName = "ot-rfsim-v3";
    std::string mEndpoint;
    uint32_t    mNodeId               = 0;
    uint64_t    mPrngState            = 1;
    uint64_t    mNowUs                = 0;
    int         mSockFd               = -1;
    bool        mDisconnected         = false;
    bool        mCompletedStartup     = false;
    bool        mPseudoResetRequested = false;
    bool        mHasPendingTasklets   = false;
    otInstance *mInstance             = nullptr;

    otRfSimCliInputCallback mCliCallback = nullptr;
    void                   *mCliContext  = nullptr;

    std::vector<uint8_t> mRxBuffer;

    // Alarm state
    std::optional<uint64_t> mAlarmMilliUs;
    std::optional<uint64_t> mAlarmMicroUs;
    std::optional<uint64_t> mLastSentAlarmUs;

    // Last pushed status (suppress duplicate kStatusPush frames)
    bool                      mHasPushedStatus = false;
    Protocol::StatusInfoValue mLastStatus      = {};

    // Radio state
    otRadioState                                 mRadioState       = OT_RADIO_STATE_DISABLED;
    uint8_t                                      mChannel          = 11;
    otPanId                                      mPanId            = 0xFFFF;
    bool                                         mPromiscuous      = false;
    int8_t                                       mTxPowerDbm       = 0;
    int8_t                                       mRxSensitivityDbm = -100;
    int8_t                                       mCcaThresholdDbm  = -75;
    int8_t                                       mLastRxRssiDbm    = -95;
    bool                                         mMacFcReset       = false;
    uint64_t                                     mNextTxMsgId      = 1;
    otRadioContext                               mRadioCtx         = {};
    std::array<uint8_t, OT_RADIO_FRAME_MAX_SIZE> mTxPsdu           = {};
    otRadioFrame                                 mTxFrame          = {};
    bool                                         mSrcMatchEnabled  = false;
    std::vector<otShortAddress>                  mSrcMatchShort;
    std::vector<std::array<uint8_t, 8>>          mSrcMatchExt;

    std::map<uint16_t, std::vector<std::vector<uint8_t>>> mSettings;

    // Spinel / CLI UART state (RCP & library modes)
    bool           mUartEnabled = false;
    int            mUartInFd    = -1;
    int            mUartOutFd   = -1;
    const uint8_t *mUartTxBuf   = nullptr;
    uint16_t       mUartTxLen   = 0;
    std::string    mUartEchoExpected;
    size_t         mUartEchoOffset = 0;

    // Virtual backbone state (TREL / Infra-If AIL / Host UDP)
    bool                  mTrelEnabled           = false;
    uint16_t              mTrelPort              = 0;
    otPlatTrelCounters    mTrelCounters          = {};
    bool                  mInfraIfEnabled        = false;
    uint32_t              mInfraIfIndex          = 1;
    bool                  mDhcp6PdListening      = false;
    otRfSimPacketCallback mTrelPacketCallback    = nullptr;
    void                 *mTrelPacketContext     = nullptr;
    otRfSimPacketCallback mInfraIfPacketCallback = nullptr;
    void                 *mInfraIfPacketContext  = nullptr;
    otRfSimPacketCallback mHostUdpPacketCallback = nullptr;
    void                 *mHostUdpPacketContext  = nullptr;
};

} // namespace RfSim
} // namespace otns

#endif // OTNS_OT_RFSIM_CLIENT_HPP_
