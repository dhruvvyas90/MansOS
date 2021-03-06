/*
 * Copyright (c) 2012 the MansOS team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of  conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//
// SAD routing, base station functionality
//

#include "../mac.h"
#include "../routing.h"
#include "../socket.h"
#include <alarms.h>
#include <timing.h>
#include <random.h>
#include <print.h>
#include <net/net_stats.h>

static Socket_t roSocket;
static Alarm_t roOriginateTimer;
static Seqnum_t mySeqnum;

static uint8_t originateRetries;

static void roOriginateTimerCb(void *);
static void routingReceive(Socket_t *s, uint8_t *data, uint16_t len);

uint64_t lastRootSyncMilliseconds;
uint64_t lastRootClockMilliseconds;

// this can serve just one downstream; use this specific address as the nexthop!
static uint16_t downstreamAddress = MOS_ADDR_BROADCAST;

// -----------------------------------------------

void routingInit(void) {
    socketOpen(&roSocket, routingReceive);
    socketBind(&roSocket, ROUTING_PROTOCOL_PORT);
    socketSetDstAddress(&roSocket, MOS_ADDR_BROADCAST);

    alarmInit(&roOriginateTimer, roOriginateTimerCb, NULL);
    alarmSchedule(&roOriginateTimer, 500);

    radioOn();
}

static void roOriginateTimerCb(void *x) {
//    TPRINTF("RO (downstream %#04x)\n", downstreamAddress);
    TPRINTF("RO\n");

    RoutingInfoPacket_t routingInfo;
    routingInfo.packetType = ROUTING_INFORMATION;
    routingInfo.senderType = SENDER_BS;
    routingInfo.rootAddress = localAddress;
    routingInfo.hopCount = 1;
    routingInfo.seqnum = ++mySeqnum;
    routingInfo.moteNumber = 0;
    if (lastRootSyncMilliseconds) {
        routingInfo.rootClockMs = lastRootClockMilliseconds
                + (getTimeMs64() - lastRootSyncMilliseconds);
    } else {
        routingInfo.rootClockMs = getTimeMs64();
    }

    socketSendEx(&roSocket, &routingInfo, sizeof(routingInfo), downstreamAddress);

    uint32_t newTime;
    if (originateRetries)  {
        originateRetries--;
        newTime = 400;
    } else {
#ifdef ROUTING_ORIGINATE_TIMEOUT
        newTime = ROUTING_ORIGINATE_TIMEOUT + randomNumberBounded(500);
#else
        newTime = timeToNextFrame() + randomInRange(200, 1000);
#endif
        if  (downstreamAddress == MOS_ADDR_BROADCAST) {
            // ARQ not possible; resend even unsolicited packets to increase reliability
            originateRetries = 2;
        }
    }
    alarmSchedule(&roOriginateTimer, newTime);
}

static void routingReceive(Socket_t *s, uint8_t *data, uint16_t len)
{
//    TPRINTF("RR %d bytes\n", len);
    if (len < 2) {
        PRINTF("routingReceive: too short!\n");
        return;
    }

    uint8_t type = *data;
    if (type == ROUTING_REQUEST) {
        // always resend solicited packets
        originateRetries = 2;
        // reschedule the origination timer sooner
        if (getAlarmTime(&roOriginateTimer) > 1200) {
            alarmSchedule(&roOriginateTimer, randomInRange(800, 1200));
        }
    }

// commented out: will not work correctly in case of multiple forwarders
#if 0
#if !SINGLE_HOP
    // set forwarder address to increase communication reliability
    uint8_t sender = *(data + 1);
    // accept collector as downstream too - for smaller networks
    if ((sender == SENDER_COLLECTOR ||  sender == SENDER_FORWARDER)
            && s->recvMacInfo->originalSrc.shortAddr) {
        downstreamAddress = s->recvMacInfo->originalSrc.shortAddr;
    }
#endif // !SINGLE_HOP
#endif // 0
}

RoutingDecision_e routePacket(MacInfo_t *info)
{
    // This is simple. Base station never forwards packets,
    // just sends and receives.
    MosAddr *dst = &info->originalDst;
    // if (!IS_LOCAL(info) && info->immedSrc.shortAddr) {
    //     downstreamAddress = info->immedSrc.shortAddr;
    // }
    fillLocalAddress(&info->immedSrc);

    // PRINTF("dst address=0x%04x, nexthop=0x%04x\n", dst->shortAddr,
    //         info->immedDst.shortAddr);
    // PRINTF("  localAddress=0x%04x\n", localAddress);

    if (isLocalAddress(dst)) {
        INC_NETSTAT(NETSTAT_PACKETS_RECV, info->originalSrc.shortAddr);
        return RD_LOCAL;
    } else {
//        PRINTF("route packet\n");
    }
    // allow to receive packets sent to "root" (0x0000) too
    if (isBroadcast(dst) || isUnspecified(dst)) {
        if (!IS_LOCAL(info)){
            INC_NETSTAT(NETSTAT_PACKETS_RECV, info->originalSrc.shortAddr);
        }
        // don't forward broadcast packets
        return IS_LOCAL(info) ? RD_BROADCAST : RD_LOCAL;
    }
    // don't forward unicast packets either
    return IS_LOCAL(info) ? RD_UNICAST : RD_DROP;
}
