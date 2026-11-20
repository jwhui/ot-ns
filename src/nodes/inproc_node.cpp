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

#include "nodes/inproc_node.hpp"

#include <openthread/dataset.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/tasklet.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <openthread/platform/alarm-micro.h>
#include <openthread/platform/alarm-milli.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "core/simulation.hpp"

namespace otns {
namespace Nodes {

namespace {

static constexpr size_t kMaxStoredLogs = 256;

std::unordered_map<otInstance *, InProcNode *> &GetInstanceMap(void)
{
    static std::unordered_map<otInstance *, InProcNode *> sMap;

    return sMap;
}

uint16_t UpdateCrc16Ccitt(uint16_t aCrc, uint8_t aByte)
{
    uint16_t q = (aCrc ^ aByte) & 0x0Fu;

    aCrc = static_cast<uint16_t>((aCrc >> 4u) ^ (q * 0x1081u));
    q    = (aCrc ^ (aByte >> 4u)) & 0x0Fu;
    aCrc = static_cast<uint16_t>((aCrc >> 4u) ^ (q * 0x1081u));

    return aCrc;
}

void WriteFcs16(otRadioFrame &aFrame)
{
    uint16_t fcs = 0;

    if (aFrame.mLength < 2)
    {
        return;
    }

    for (uint16_t i = 0; i < aFrame.mLength - 2u; ++i)
    {
        fcs = UpdateCrc16Ccitt(fcs, aFrame.mPsdu[i]);
    }

    aFrame.mPsdu[aFrame.mLength - 2u] = static_cast<uint8_t>(fcs & 0xFFu);
    aFrame.mPsdu[aFrame.mLength - 1u] = static_cast<uint8_t>((fcs >> 8u) & 0xFFu);
}

Rf::RadioState ToRfState(otRadioState aState)
{
    switch (aState)
    {
    case OT_RADIO_STATE_DISABLED:
        return Rf::RadioState::kDisabled;
    case OT_RADIO_STATE_SLEEP:
        return Rf::RadioState::kSleep;
    case OT_RADIO_STATE_RECEIVE:
        return Rf::RadioState::kRx;
    case OT_RADIO_STATE_TRANSMIT:
        return Rf::RadioState::kTx;
    default:
        return Rf::RadioState::kDisabled;
    }
}

Core::DeviceRole ToDeviceRole(otDeviceRole aRole)
{
    switch (aRole)
    {
    case OT_DEVICE_ROLE_DISABLED:
        return Core::DeviceRole::kDisabled;
    case OT_DEVICE_ROLE_DETACHED:
        return Core::DeviceRole::kDetached;
    case OT_DEVICE_ROLE_CHILD:
        return Core::DeviceRole::kChild;
    case OT_DEVICE_ROLE_ROUTER:
        return Core::DeviceRole::kRouter;
    case OT_DEVICE_ROLE_LEADER:
        return Core::DeviceRole::kLeader;
    default:
        return Core::DeviceRole::kDisabled;
    }
}

bool ParseHexBytes(const std::string &aHex, uint8_t *aOut, size_t aExpectedBytes)
{
    if (aHex.size() != aExpectedBytes * 2)
    {
        return false;
    }

    auto parseNibble = [](char aCh) -> int {
        if (aCh >= '0' && aCh <= '9')
        {
            return aCh - '0';
        }
        if (aCh >= 'a' && aCh <= 'f')
        {
            return 10 + (aCh - 'a');
        }
        if (aCh >= 'A' && aCh <= 'F')
        {
            return 10 + (aCh - 'A');
        }
        return -1;
    };

    for (size_t i = 0; i < aExpectedBytes; ++i)
    {
        int hi = parseNibble(aHex[i * 2]);
        int lo = parseNibble(aHex[i * 2 + 1]);

        if (hi < 0 || lo < 0)
        {
            return false;
        }
        aOut[i] = static_cast<uint8_t>((hi << 4) | lo);
    }

    return true;
}

} // namespace

InProcNode *InProcNode::FromInstance(otInstance *aInstance)
{
    auto &map = GetInstanceMap();
    auto  it  = map.find(aInstance);

    return (it != map.end()) ? it->second : nullptr;
}

InProcNode::InProcNode(Core::Simulation &aSim, Rf::NodeId aId, const Core::NodeConfig &aConfig)
    : mSim(aSim)
    , mId(aId)
    , mPos(aConfig.mPos)
    , mRadioRange(aConfig.mRadioRange)
    , mLabel(aConfig.mLabel)
    , mAliveFlag(std::make_shared<bool>(true))
    , mChannel(aSim.GetConfig().mChannel)
{
    std::memset(&mRadioCtx, 0, sizeof(mRadioCtx));
    mRadioCtx.mShortAddress          = OT_RADIO_INVALID_SHORT_ADDR;
    mRadioCtx.mAlternateShortAddress = OT_RADIO_INVALID_SHORT_ADDR;

    std::memset(&mTxFrame, 0, sizeof(mTxFrame));
    mTxFrame.mPsdu = mTxPsdu.data();

    // Query required OpenThread instance size and allocate aligned buffer.
    size_t instSize = 0;

    (void)otInstanceInit(nullptr, &instSize);
    mInstanceBuf.resize(instSize, 0);

    // Register buffer address before otInstanceInit() so any otPlat* calls during init resolve this node.
    auto *rawInst = reinterpret_cast<otInstance *>(mInstanceBuf.data());

    GetInstanceMap()[rawInst] = this;

    mOtInstance = otInstanceInit(mInstanceBuf.data(), &instSize);
    if (mOtInstance != rawInst)
    {
        GetInstanceMap().erase(rawInst);
        GetInstanceMap()[mOtInstance] = this;
    }

    // Initialize per-instance CLI interpreter using OpenThread's public multi-instance CLI API.
    size_t cliSize = otCliInterpreterGetSize();

    mCliBuf.resize(cliSize, 0);
    mCliInterp = otCliInterpreterInit(mCliBuf.data(), mCliBuf.size(), mOtInstance, &InProcNode::HandleCliOutput, this);
#if OPENTHREAD_CONFIG_CLI_PROMPT_ENABLE
    otCliInterpreterSetPromptConfig(mCliInterp, false);
#endif

    if (aConfig.mAutoStart)
    {
        ConfigureInitialDataset(aConfig);
    }
}

InProcNode::~InProcNode(void)
{
    *mAliveFlag = false;
    StopAlarmMilli();
    StopAlarmMicro();

    if (mCliInterp != nullptr)
    {
        otCliInterpreterFinalize(mCliInterp);
        mCliInterp = nullptr;
    }

    if (mOtInstance != nullptr)
    {
        otInstanceFinalize(mOtInstance);
        GetInstanceMap().erase(mOtInstance);
        mOtInstance = nullptr;
    }
}

void InProcNode::ConfigureInitialDataset(const Core::NodeConfig &aConfig)
{
    static constexpr uint8_t kExtPanId[8] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22};
    static constexpr char    kNetName[]   = "OTNSv3";

    otOperationalDataset dataset;
    otLinkModeConfig     mode;

    std::memset(&dataset, 0, sizeof(dataset));

    dataset.mActiveTimestamp.mSeconds             = 1;
    dataset.mComponents.mIsActiveTimestampPresent = true;

    dataset.mChannel                      = mSim.GetConfig().mChannel;
    dataset.mComponents.mIsChannelPresent = true;

    dataset.mPanId                      = mSim.GetConfig().mPanId;
    dataset.mComponents.mIsPanIdPresent = true;

    std::memcpy(dataset.mNetworkName.m8, kNetName, sizeof(kNetName) - 1);
    dataset.mComponents.mIsNetworkNamePresent = true;

    std::memcpy(dataset.mExtendedPanId.m8, kExtPanId, sizeof(kExtPanId));
    dataset.mComponents.mIsExtendedPanIdPresent = true;

    if (ParseHexBytes(mSim.GetConfig().mNetworkKey, dataset.mNetworkKey.m8, OT_NETWORK_KEY_SIZE))
    {
        dataset.mComponents.mIsNetworkKeyPresent = true;
    }

    (void)otDatasetSetActive(mOtInstance, &dataset);

    std::memset(&mode, 0, sizeof(mode));
    switch (aConfig.mType)
    {
    case Core::NodeType::kRouter:
        mode.mRxOnWhenIdle = true;
        mode.mDeviceType   = true;
        mode.mNetworkData  = true;
        break;
    case Core::NodeType::kFed:
        mode.mRxOnWhenIdle = true;
        mode.mDeviceType   = true;
        mode.mNetworkData  = true;
        (void)otThreadSetRouterEligible(mOtInstance, false);
        break;
    case Core::NodeType::kMed:
        mode.mRxOnWhenIdle = true;
        mode.mDeviceType   = false;
        mode.mNetworkData  = true;
        break;
    case Core::NodeType::kSed:
        mode.mRxOnWhenIdle = false;
        mode.mDeviceType   = false;
        mode.mNetworkData  = false;
        break;
    }

    (void)otThreadSetLinkMode(mOtInstance, mode);
    (void)otIp6SetEnabled(mOtInstance, true);
    (void)otThreadSetEnabled(mOtInstance, true);
}

void InProcNode::SetEnabled(bool aEnabled)
{
    mEnabled = aEnabled;
    if (!mEnabled)
    {
        StopAlarmMilli();
        StopAlarmMicro();
        mRadioState = OT_RADIO_STATE_DISABLED;
    }
}

void InProcNode::SignalPendingTasklets(void)
{
    mHasPendingTasklets = true;
    mSim.MarkPendingTasklets();
}

void InProcNode::ProcessTasklets(void)
{
    mHasPendingTasklets = false;
    if (mEnabled && mOtInstance != nullptr)
    {
        otTaskletsProcess(mOtInstance);
    }
}

int InProcNode::HandleCliOutput(void *aContext, const char *aFormat, va_list aArgs)
{
    auto *self = static_cast<InProcNode *>(aContext);
    char  buf[1024];
    int   n = std::vsnprintf(buf, sizeof(buf), aFormat, aArgs);

    if (n > 0)
    {
        size_t toAppend = std::min(static_cast<size_t>(n), sizeof(buf) - 1);

        self->mCliOutput.append(buf, toAppend);
    }

    return n;
}

std::string InProcNode::ExecuteCli(const std::string &aCommand)
{
    mCliOutput.clear();
    if (mCliInterp == nullptr)
    {
        return "Error: CLI unavailable\n";
    }

    std::string mutableCmd = aCommand;

    otCliInterpreterInputLine(mCliInterp, mutableCmd.data());

    return TakeCliOutput();
}

std::string InProcNode::TakeCliOutput(void)
{
    std::string out;

    out.swap(mCliOutput);

    return out;
}

Core::NodeSnapshot InProcNode::GetSnapshot(void) const
{
    Core::NodeSnapshot snap;

    snap.mId         = mId;
    snap.mKind       = Core::NodeKind::kInProc;
    snap.mPos        = mPos;
    snap.mRadioRange = mRadioRange;
    snap.mLabel      = mLabel;
    snap.mEnabled    = mEnabled;

    if (mOtInstance != nullptr)
    {
        const otExtAddress *ext = otLinkGetExtendedAddress(mOtInstance);

        snap.mRole        = ToDeviceRole(otThreadGetDeviceRole(mOtInstance));
        snap.mRloc16      = otThreadGetRloc16(mOtInstance);
        snap.mPartitionId = otThreadGetPartitionId(mOtInstance);

        if (ext != nullptr)
        {
            char hex[17];

            std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x%02x%02x", ext->m8[0], ext->m8[1], ext->m8[2],
                          ext->m8[3], ext->m8[4], ext->m8[5], ext->m8[6], ext->m8[7]);
            snap.mExtAddr = hex;
        }

        if (snap.mRole == Core::DeviceRole::kChild)
        {
            otRouterInfo parentInfo;

            std::memset(&parentInfo, 0, sizeof(parentInfo));
            if (otThreadGetParentInfo(mOtInstance, &parentInfo) == OT_ERROR_NONE)
            {
                snap.mParentRloc16 = parentInfo.mRloc16;
            }
        }

        otNeighborInfoIterator iter = OT_NEIGHBOR_INFO_ITERATOR_INIT;
        otNeighborInfo         ninfo;

        while (otThreadGetNextNeighborInfo(mOtInstance, &iter, &ninfo) == OT_ERROR_NONE)
        {
            Core::NeighborLinkSnapshot link;

            link.mRloc16  = ninfo.mRloc16;
            link.mRssiDbm = ninfo.mAverageRssi;
            link.mLqi     = ninfo.mLinkQualityIn;
            link.mIsChild = ninfo.mIsChild;
            snap.mNeighbors.push_back(link);
        }
    }

    return snap;
}

void InProcNode::StartAlarmMilliAt(uint32_t aT0Ms, uint32_t aDtMs)
{
    StopAlarmMilli();
    if (!mEnabled)
    {
        return;
    }

    uint64_t nowUs    = mSim.GetNowUs();
    uint64_t nowMs    = nowUs / 1000ULL;
    uint32_t nowMs32  = static_cast<uint32_t>(nowMs);
    int32_t  deltaMs  = static_cast<int32_t>((aT0Ms + aDtMs) - nowMs32);
    uint64_t targetUs = (deltaMs <= 0) ? nowUs : ((nowMs + static_cast<uint64_t>(deltaMs)) * 1000ULL);
    auto     alive    = mAliveFlag;

    mAlarmMilliId = mSim.GetScheduler().ScheduleAt(targetUs, [this, alive](void) {
        if (!*alive || !mEnabled)
        {
            return;
        }
        mAlarmMilliId = Core::kInvalidEventId;
        otPlatAlarmMilliFired(mOtInstance);
    });
}

void InProcNode::StopAlarmMilli(void)
{
    if (mAlarmMilliId != Core::kInvalidEventId)
    {
        mSim.GetScheduler().Cancel(mAlarmMilliId);
        mAlarmMilliId = Core::kInvalidEventId;
    }
}

void InProcNode::StartAlarmMicroAt(uint32_t aT0Us, uint32_t aDtUs)
{
    StopAlarmMicro();
    if (!mEnabled)
    {
        return;
    }

    uint64_t nowUs    = mSim.GetNowUs();
    uint32_t nowUs32  = static_cast<uint32_t>(nowUs);
    int32_t  deltaUs  = static_cast<int32_t>((aT0Us + aDtUs) - nowUs32);
    uint64_t targetUs = (deltaUs <= 0) ? nowUs : (nowUs + static_cast<uint64_t>(deltaUs));
    auto     alive    = mAliveFlag;

    mAlarmMicroId = mSim.GetScheduler().ScheduleAt(targetUs, [this, alive](void) {
        if (!*alive || !mEnabled)
        {
            return;
        }
        mAlarmMicroId = Core::kInvalidEventId;
        otPlatAlarmMicroFired(mOtInstance);
    });
}

void InProcNode::StopAlarmMicro(void)
{
    if (mAlarmMicroId != Core::kInvalidEventId)
    {
        mSim.GetScheduler().Cancel(mAlarmMicroId);
        mAlarmMicroId = Core::kInvalidEventId;
    }
}

otError InProcNode::RadioEnable(void)
{
    if (mRadioState != OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_FAILED;
    }

    mRadioState = OT_RADIO_STATE_SLEEP;
    mSim.GetRadioModel().SetNodeRadioState(mId, ToRfState(mRadioState), mChannel);

    return OT_ERROR_NONE;
}

otError InProcNode::RadioDisable(void)
{
    mRadioState = OT_RADIO_STATE_DISABLED;
    mSim.GetRadioModel().SetNodeRadioState(mId, ToRfState(mRadioState), mChannel);

    return OT_ERROR_NONE;
}

otError InProcNode::RadioSleep(void)
{
    if (mRadioState == OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_INVALID_STATE;
    }
    if (mRadioState == OT_RADIO_STATE_TRANSMIT)
    {
        return OT_ERROR_BUSY;
    }

    mRadioState = OT_RADIO_STATE_SLEEP;
    mSim.GetRadioModel().SetNodeRadioState(mId, ToRfState(mRadioState), mChannel);

    return OT_ERROR_NONE;
}

otError InProcNode::RadioReceive(uint8_t aChannel)
{
    if (mRadioState == OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_INVALID_STATE;
    }

    mChannel    = aChannel;
    mRadioState = OT_RADIO_STATE_RECEIVE;
    mSim.GetRadioModel().SetNodeRadioState(mId, ToRfState(mRadioState), mChannel);

    return OT_ERROR_NONE;
}

void InProcNode::SetExtendedAddress(const otExtAddress *aExtAddr)
{
    if (aExtAddr == nullptr)
    {
        return;
    }

    // otRadioContext::mExtAddress is stored in little-endian byte order.
    for (size_t i = 0; i < 8; ++i)
    {
        mRadioCtx.mExtAddress.m8[i] = aExtAddr->m8[7 - i];
    }
}

void InProcNode::SetTxPower(int8_t aPowerDbm)
{
    mTxPowerDbm = aPowerDbm;
    mSim.GetRadioModel().SetNodeTxPower(mId, static_cast<Rf::DbValue>(aPowerDbm));
}

otError InProcNode::AddSrcMatchShort(otShortAddress aAddr)
{
    if (std::find(mSrcMatchShort.begin(), mSrcMatchShort.end(), aAddr) == mSrcMatchShort.end())
    {
        mSrcMatchShort.push_back(aAddr);
    }

    return OT_ERROR_NONE;
}

otError InProcNode::AddSrcMatchExt(const otExtAddress *aAddr)
{
    std::array<uint8_t, 8> rev{};

    if (aAddr == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    for (size_t i = 0; i < 8; ++i)
    {
        rev[i] = aAddr->m8[7 - i];
    }

    if (std::find(mSrcMatchExt.begin(), mSrcMatchExt.end(), rev) == mSrcMatchExt.end())
    {
        mSrcMatchExt.push_back(rev);
    }

    return OT_ERROR_NONE;
}

otError InProcNode::ClearSrcMatchShort(otShortAddress aAddr)
{
    auto it = std::find(mSrcMatchShort.begin(), mSrcMatchShort.end(), aAddr);

    if (it == mSrcMatchShort.end())
    {
        return OT_ERROR_NO_ADDRESS;
    }

    mSrcMatchShort.erase(it);

    return OT_ERROR_NONE;
}

otError InProcNode::ClearSrcMatchExt(const otExtAddress *aAddr)
{
    std::array<uint8_t, 8> rev{};

    if (aAddr == nullptr)
    {
        return OT_ERROR_INVALID_ARGS;
    }

    for (size_t i = 0; i < 8; ++i)
    {
        rev[i] = aAddr->m8[7 - i];
    }

    auto it = std::find(mSrcMatchExt.begin(), mSrcMatchExt.end(), rev);

    if (it == mSrcMatchExt.end())
    {
        return OT_ERROR_NO_ADDRESS;
    }

    mSrcMatchExt.erase(it);

    return OT_ERROR_NONE;
}

bool InProcNode::HasFramePendingFor(const otMacAddress &aSrcAddr) const
{
    if (!mSrcMatchEnabled)
    {
        return true;
    }

    if (aSrcAddr.mType == OT_MAC_ADDRESS_TYPE_SHORT)
    {
        return std::find(mSrcMatchShort.begin(), mSrcMatchShort.end(), aSrcAddr.mAddress.mShortAddress) !=
               mSrcMatchShort.end();
    }

    if (aSrcAddr.mType == OT_MAC_ADDRESS_TYPE_EXTENDED)
    {
        std::array<uint8_t, 8> key{};

        std::memcpy(key.data(), aSrcAddr.mAddress.mExtAddress.m8, 8);

        return std::find(mSrcMatchExt.begin(), mSrcMatchExt.end(), key) != mSrcMatchExt.end();
    }

    return false;
}

void InProcNode::SetMacKey(uint8_t                 aKeyIdMode,
                           uint8_t                 aKeyIndex,
                           const otMacKeyMaterial *aPrevKey,
                           const otMacKeyMaterial *aCurrKey,
                           const otMacKeyMaterial *aNextKey,
                           otRadioKeyType          aKeyType)
{
    (void)aKeyIdMode;

    mRadioCtx.mKeyId   = aKeyIndex;
    mRadioCtx.mKeyType = aKeyType;

    if (!mMacFcReset)
    {
        mRadioCtx.mPrevMacFrameCounter = mRadioCtx.mMacFrameCounter;
        mRadioCtx.mMacFrameCounter     = 0;
    }
    mMacFcReset = false;

    if (aPrevKey != nullptr)
    {
        std::memcpy(&mRadioCtx.mPrevKey, aPrevKey, sizeof(otMacKeyMaterial));
    }
    if (aCurrKey != nullptr)
    {
        std::memcpy(&mRadioCtx.mCurrKey, aCurrKey, sizeof(otMacKeyMaterial));
    }
    if (aNextKey != nullptr)
    {
        std::memcpy(&mRadioCtx.mNextKey, aNextKey, sizeof(otMacKeyMaterial));
    }
}

void InProcNode::SetMacFrameCounter(uint32_t aCounter)
{
    if (aCounter == 0)
    {
        mRadioCtx.mPrevMacFrameCounter = mRadioCtx.mMacFrameCounter;
        mMacFcReset                    = true;
    }
    mRadioCtx.mMacFrameCounter = aCounter;
}

void InProcNode::SetMacFrameCounterIfLarger(uint32_t aCounter)
{
    if (aCounter > mRadioCtx.mMacFrameCounter)
    {
        mRadioCtx.mMacFrameCounter = aCounter;
    }
}

void InProcNode::SetCslParams(uint32_t aPeriod, otShortAddress aShortAddr, const otExtAddress *aExtAddr)
{
    mRadioCtx.mCslPeriod       = static_cast<uint16_t>(aPeriod);
    mRadioCtx.mCslShortAddress = aShortAddr;
    if (aExtAddr != nullptr)
    {
        for (size_t i = 0; i < 8; ++i)
        {
            mRadioCtx.mCslExtAddress.m8[i] = aExtAddr->m8[7 - i];
        }
    }
}

otError InProcNode::RadioTransmit(otRadioFrame *aFrame)
{
    if (mRadioState != OT_RADIO_STATE_RECEIVE || aFrame != &mTxFrame)
    {
        return OT_ERROR_INVALID_STATE;
    }

    mChannel    = aFrame->mChannel;
    mRadioState = OT_RADIO_STATE_TRANSMIT;
    mSim.GetRadioModel().SetNodeRadioState(mId, Rf::RadioState::kTx, mChannel);

    if (otMacFrameProcessTxSfd(aFrame, mSim.GetNowUs(), &mRadioCtx) != OT_ERROR_NONE)
    {
        mRadioState = OT_RADIO_STATE_RECEIVE;
        mSim.GetRadioModel().SetNodeRadioState(mId, Rf::RadioState::kRx, mChannel);
        return OT_ERROR_ABORT;
    }

    WriteFcs16(*aFrame);
    otPlatRadioTxStarted(mOtInstance, aFrame);

    Core::RawFrame raw;

    raw.mChannel     = aFrame->mChannel;
    raw.mTimestampUs = mSim.GetNowUs();
    raw.mPsdu.assign(aFrame->mPsdu, aFrame->mPsdu + aFrame->mLength);

    otMacAddress dstAddr;
    bool         hasDst  = (otMacFrameGetDstAddr(aFrame, &dstAddr) == OT_ERROR_NONE);
    bool         isBcast = !hasDst || (dstAddr.mType == OT_MAC_ADDRESS_TYPE_NONE) ||
                   (dstAddr.mType == OT_MAC_ADDRESS_TYPE_SHORT && dstAddr.mAddress.mShortAddress == 0xFFFF);
    bool ackReq = otMacFrameIsAckRequested(aFrame) && !isBcast;
    auto alive  = mAliveFlag;

    mSim.StartRadioTx(mId, raw, ackReq, aFrame->mInfo.mTxInfo.mCsmaCaEnabled, mCcaThresholdDbm,
                      static_cast<Rf::DbValue>(mTxPowerDbm),
                      [this, alive](const std::optional<Core::RawFrame> &aAck, uint8_t aErr) {
                          if (!*alive || !mEnabled)
                          {
                              return;
                          }
                          mRadioState = OT_RADIO_STATE_RECEIVE;
                          mSim.GetRadioModel().SetNodeRadioState(mId, Rf::RadioState::kRx, mChannel);

                          if (aAck.has_value() && aErr == OT_ERROR_NONE)
                          {
                              std::array<uint8_t, OT_RADIO_FRAME_MAX_SIZE> ackBuf{};
                              size_t       len = std::min(aAck->mPsdu.size(), ackBuf.size());
                              otRadioFrame otAck;

                              std::memcpy(ackBuf.data(), aAck->mPsdu.data(), len);
                              std::memset(&otAck, 0, sizeof(otAck));
                              otAck.mPsdu                    = ackBuf.data();
                              otAck.mLength                  = static_cast<uint16_t>(len);
                              otAck.mChannel                 = aAck->mChannel;
                              otAck.mInfo.mRxInfo.mTimestamp = aAck->mTimestampUs;
                              otAck.mInfo.mRxInfo.mRssi      = aAck->mRssiDbm;
                              otAck.mInfo.mRxInfo.mLqi       = aAck->mLqi;
                              otPlatRadioTxDone(mOtInstance, &mTxFrame, &otAck, OT_ERROR_NONE);
                          }
                          else
                          {
                              otPlatRadioTxDone(mOtInstance, &mTxFrame, nullptr, static_cast<otError>(aErr));
                          }
                      });

    return OT_ERROR_NONE;
}

otError InProcNode::RadioEnergyScan(uint8_t aChannel, uint16_t aDurationMs)
{
    if (mRadioState == OT_RADIO_STATE_DISABLED)
    {
        return OT_ERROR_INVALID_STATE;
    }

    auto alive = mAliveFlag;

    mSim.StartEnergyScan(mId, aChannel, static_cast<uint64_t>(aDurationMs) * 1000ULL, [this, alive](int8_t aPeakRssi) {
        if (!*alive || !mEnabled)
        {
            return;
        }
        otPlatRadioEnergyScanDone(mOtInstance, aPeakRssi);
    });

    return OT_ERROR_NONE;
}

bool InProcNode::OnRadioRxDone(const Core::RawFrame &aRxFrame, Core::RawFrame &aAckOut)
{
    if (!mEnabled || mRadioState != OT_RADIO_STATE_RECEIVE || aRxFrame.mChannel != mChannel)
    {
        return false;
    }
    if (aRxFrame.mPsdu.size() < 2 || aRxFrame.mPsdu.size() > OT_RADIO_FRAME_MAX_SIZE)
    {
        return false;
    }

    std::array<uint8_t, OT_RADIO_FRAME_MAX_SIZE> psduCopy{};
    otRadioFrame                                 otRx;

    std::memcpy(psduCopy.data(), aRxFrame.mPsdu.data(), aRxFrame.mPsdu.size());
    std::memset(&otRx, 0, sizeof(otRx));
    otRx.mPsdu                    = psduCopy.data();
    otRx.mLength                  = static_cast<uint16_t>(aRxFrame.mPsdu.size());
    otRx.mChannel                 = aRxFrame.mChannel;
    otRx.mInfo.mRxInfo.mTimestamp = aRxFrame.mTimestampUs;
    otRx.mInfo.mRxInfo.mRssi      = aRxFrame.mRssiDbm;
    otRx.mInfo.mRxInfo.mLqi       = aRxFrame.mLqi;

    bool addrMatch = otMacFrameDoesAddrMatchAny(&otRx, mPanId, mRadioCtx.mShortAddress,
                                                mRadioCtx.mAlternateShortAddress, &mRadioCtx.mExtAddress);

    if (!addrMatch && !mPromiscuous)
    {
        return false;
    }

    bool shouldAck = false;

    if (addrMatch && otMacFrameIsAckRequested(&otRx))
    {
        otMacAddress dstAddr;
        bool         hasDst    = (otMacFrameGetDstAddr(&otRx, &dstAddr) == OT_ERROR_NONE);
        bool         isUnicast = hasDst && dstAddr.mType != OT_MAC_ADDRESS_TYPE_NONE &&
                         !(dstAddr.mType == OT_MAC_ADDRESS_TYPE_SHORT && dstAddr.mAddress.mShortAddress == 0xFFFF);

        if (isUnicast)
        {
            bool framePending = false;

            if (otMacFrameIsDataRequest(&otRx))
            {
                otMacAddress srcAddr;

                if (otMacFrameGetSrcAddr(&otRx, &srcAddr) == OT_ERROR_NONE)
                {
                    framePending = HasFramePendingFor(srcAddr);
                }
            }
            otRx.mInfo.mRxInfo.mAckedWithFramePending = framePending;

            std::array<uint8_t, OT_RADIO_FRAME_MAX_SIZE> ackBuf{};
            otRadioFrame                                 otAck;
            bool                                         ackOk = false;

            std::memset(&otAck, 0, sizeof(otAck));
            otAck.mPsdu    = ackBuf.data();
            otAck.mChannel = mChannel;

            if (otMacFrameIsVersion2015(&otRx))
            {
                uint8_t ieData[OT_ACK_IE_MAX_SIZE];
                uint8_t ieLen = 0;

#if OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE
                if (mRadioCtx.mCslPeriod > 0 && otMacFrameSrcAddrMatchCslReceiverPeer(&otRx, &mRadioCtx))
                {
                    ieLen = otMacFrameGenerateCslIeTemplate(ieData);
                }
#endif
                if (otMacFrameGenerateEnhAck(&otRx, framePending, ieData, ieLen, &otAck) == OT_ERROR_NONE &&
                    otMacFrameProcessTxSfd(&otAck, mSim.GetNowUs(), &mRadioCtx) == OT_ERROR_NONE)
                {
                    ackOk = true;
                }
            }
            else
            {
                otMacFrameGenerateImmAck(&otRx, framePending, &otAck);
                ackOk = true;
            }

            if (ackOk)
            {
                WriteFcs16(otAck);
                aAckOut.mChannel = mChannel;
                aAckOut.mPsdu.assign(otAck.mPsdu, otAck.mPsdu + otAck.mLength);
                shouldAck = true;
            }
        }
    }

    otPlatRadioReceiveDone(mOtInstance, &otRx, OT_ERROR_NONE);

    return shouldAck;
}

otError InProcNode::SettingsGet(uint16_t aKey, int aIndex, uint8_t *aValue, uint16_t *aValueLen) const
{
    auto it = mSettings.find(aKey);

    if (it == mSettings.end() || aIndex < 0 || static_cast<size_t>(aIndex) >= it->second.size())
    {
        return OT_ERROR_NOT_FOUND;
    }

    const auto &entry = it->second[static_cast<size_t>(aIndex)];

    if (aValueLen != nullptr)
    {
        uint16_t maxLen = *aValueLen;
        uint16_t len    = static_cast<uint16_t>(entry.size());

        *aValueLen = len;
        if (aValue != nullptr && len > 0)
        {
            std::memcpy(aValue, entry.data(), std::min(maxLen, len));
        }
    }

    return OT_ERROR_NONE;
}

otError InProcNode::SettingsSet(uint16_t aKey, const uint8_t *aValue, uint16_t aValueLen)
{
    auto                &vec = mSettings[aKey];
    std::vector<uint8_t> bytes;

    vec.clear();
    if (aValue != nullptr && aValueLen > 0)
    {
        bytes.assign(aValue, aValue + aValueLen);
    }
    vec.push_back(std::move(bytes));

    return OT_ERROR_NONE;
}

otError InProcNode::SettingsAdd(uint16_t aKey, const uint8_t *aValue, uint16_t aValueLen)
{
    auto                &vec = mSettings[aKey];
    std::vector<uint8_t> bytes;

    if (aValue != nullptr && aValueLen > 0)
    {
        bytes.assign(aValue, aValue + aValueLen);
    }
    // OpenThread convention: newly added entries appear at index 0.
    vec.insert(vec.begin(), std::move(bytes));

    return OT_ERROR_NONE;
}

otError InProcNode::SettingsDelete(uint16_t aKey, int aIndex)
{
    auto it = mSettings.find(aKey);

    if (it == mSettings.end())
    {
        return OT_ERROR_NOT_FOUND;
    }

    if (aIndex < 0)
    {
        mSettings.erase(it);
        return OT_ERROR_NONE;
    }

    if (static_cast<size_t>(aIndex) >= it->second.size())
    {
        return OT_ERROR_NOT_FOUND;
    }

    it->second.erase(it->second.begin() + aIndex);
    if (it->second.empty())
    {
        mSettings.erase(it);
    }

    return OT_ERROR_NONE;
}

void InProcNode::AppendLog(otLogLevel aLevel, const char *aLine)
{
    (void)aLevel;

    if (aLine == nullptr)
    {
        return;
    }

    if (mLogs.size() >= kMaxStoredLogs)
    {
        mLogs.erase(mLogs.begin());
    }
    mLogs.emplace_back(aLine);
}

} // namespace Nodes
} // namespace otns
