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
 *   Public C API for embedding the ot-rfsim v3 OpenThread platform in standalone
 *   binaries (ot-cli-ftd-rfsim) or library mode (Matter / OTBR / custom hosts).
 */

#ifndef OTNS_OT_RFSIM_V3_H_
#define OTNS_OT_RFSIM_V3_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <openthread/instance.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*otRfSimCliInputCallback)(const char *aLine, void *aContext);

typedef void (*otRfSimPacketCallback)(const uint8_t  aSrcIp6[16],
                                      uint16_t       aSrcPort,
                                      const uint8_t  aDstIp6[16],
                                      uint16_t       aDstPort,
                                      const uint8_t *aPayload,
                                      uint16_t       aLength,
                                      void          *aContext);

/**
 * Sets an optional application label sent during the v3 Hello handshake
 * (e.g., "matter", "rcp", "otbr"). Must be called before otSysInit() / otRfSimConnect().
 */
void otRfSimSetAppName(const char *aName);

/**
 * Configures the simulator socket endpoint before otSysInit() or otRfSimConnect().
 *
 * @param[in] aEndpoint  Socket URI: "unix:///path/to.sock", "/path/to.sock", or "tcp://127.0.0.1:PORT".
 * @param[in] aNodeId    Requested node ID (0 to let the simulator assign one).
 * @param[in] aSeed      Optional local seed fallback before HelloAck assigns a deterministic seed.
 */
void otRfSimConfigure(const char *aEndpoint, uint32_t aNodeId, uint64_t aSeed);

/**
 * Connects to the configured simulator endpoint and completes the v3 Hello/HelloAck handshake.
 */
bool otRfSimConnect(void);

/**
 * Sends Shutdown and closes the simulator socket.
 */
void otRfSimDisconnect(void);

/**
 * Returns true if the simulator socket is currently connected.
 */
bool otRfSimIsConnected(void);

/**
 * Returns the underlying POSIX socket FD (or -1 if disconnected) for host poll/select loops.
 */
int otRfSimGetSocketFd(void);

/**
 * Returns the node ID confirmed by the simulator in HelloAck.
 */
uint32_t otRfSimGetNodeId(void);

/**
 * Registers the OpenThread instance for status pushes and event dispatch.
 */
void otRfSimAttachInstance(otInstance *aInstance);

/**
 * Registers a callback invoked when the simulator sends a CLI command line.
 */
void otRfSimSetCliHandler(otRfSimCliInputCallback aCallback, void *aContext);

/**
 * Sends CLI output text back to the simulator.
 */
void otRfSimSendCliOutput(const char *aText, size_t aLength);

/**
 * Finishes startup synchronization by draining tasklets, syncing alarms/status,
 * and sending the initial StepDone frame.
 */
bool otRfSimCompleteStartup(otInstance *aInstance);

/**
 * Joins the simulated Adjacent Infrastructure Link (AIL / Infra-If) and notifies
 * OpenThread Border Routing if enabled.
 */
bool otRfSimInitInfraIf(otInstance *aInstance, uint32_t aInfraIfIndex);

/**
 * Leaves the simulated Adjacent Infrastructure Link (AIL / Infra-If).
 */
void otRfSimDeinitInfraIf(otInstance *aInstance);

/**
 * Computes this node's deterministic simulated AIL link-local IPv6 address (fe80::<nodeId>).
 */
bool otRfSimGetInfraIfLinkLocalAddress(uint8_t aAddr[16]);

/**
 * Computes this node's deterministic simulated virtual backbone IPv6 address (fd00:0:0:cafe::<nodeId>).
 */
bool otRfSimGetBackboneAddress(uint8_t aAddr[16]);

/**
 * Registers optional callbacks for observing or handling virtual backbone packets
 * (TREL, Infra-If AIL, and Host UDP/IPv6).
 */
void otRfSimSetTrelPacketHandler(otRfSimPacketCallback aCallback, void *aContext);
void otRfSimSetInfraIfPacketHandler(otRfSimPacketCallback aCallback, void *aContext);
void otRfSimSetHostUdpPacketHandler(otRfSimPacketCallback aCallback, void *aContext);

/**
 * Direct virtual backbone helpers for library-mode hosts (e.g., Matter controllers/devices)
 * communicating over the simulator's virtual IPv6/UDP fabric without kernel TUN/sockets.
 */
bool otRfSimRegisterTrelService(uint16_t aUdpPort, const uint8_t *aTxtData, uint16_t aTxtLength);
void otRfSimUnregisterTrelService(void);
bool otRfSimSendTrelPacket(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength);
bool otRfSimSendInfraIfPacket(const uint8_t aDstIp6[16], uint16_t aDstPort, const uint8_t *aPayload, uint16_t aLength);
bool otRfSimBindHostUdp(uint16_t aPort);
bool otRfSimUnbindHostUdp(uint16_t aPort);
bool otRfSimSendHostUdp(uint16_t       aSrcPort,
                        const uint8_t  aDstIp6[16],
                        uint16_t       aDstPort,
                        const uint8_t *aPayload,
                        uint16_t       aLength);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // OTNS_OT_RFSIM_V3_H_
