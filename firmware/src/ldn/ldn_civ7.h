/**
 * Civ VII LDN game protocol: Fire transport (reliable-ish UDP), FireCONN ->
 * NetWelcome handshake, NetRegister/NetAppRegister/NetSnapshot and NetChat
 * injection, plus the ARP warmup. Runs over UDP :62056 inside the joined
 * LDN network; IP/UDP frames are built by hand and pushed with
 * esp_wifi_internal_tx (the console never sees us on lwIP).
 *
 * SPDX-License-Identifier: GPL-3.0
 * Ported from CivSwitchLDN's ldn/{protocol,transport,lobby}.py
 * (GPL-3.0-only) via SeanHeuc's esp32-ldn-civ7 (main/civ7.c, © Sean Heuc),
 * spike-proven (ticket 09). Ticket 10.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ldn.h"

namespace ldn {

/** One chat line per call: fill buf (NUL-terminated not required), return
 * its length; 0 = end of lines, <0 = read error. */
typedef int (*NextChatLineFn)(int index, char *buf, size_t cap, void *user);

void civ7SetNet(const uint8_t localIp[4], const uint8_t hostIp[4],
                const uint8_t hostMac[6], const uint8_t staMac[6]);

/** Feed a UDP payload the console addressed to us (dport 62056). Callable
 * from the wifi rx callback: only touches volatile state, never logs. */
void civ7RxUdp(const uint8_t *data, uint16_t len);

/** Raw UDP datagram to the console inside the LDN link. */
int civ7UdpSend(uint16_t port, const uint8_t *data, size_t len);

uint32_t civ7Slot();
bool civ7Welcome();

/** ARP warmup + FireCONN retried until FireRPLY/NetWelcome (blocking,
 * ~up to 18 s). false with *err set when the lobby never answers. */
bool civ7Handshake(LogFn log, YieldFn yieldUi, String *err);

/** NetRegister / NetAppRegister / NetSnapshot, then the chat lines (200 ms
 * apart, CivSwitchLDN net_chat_public). Blocking. */
bool civ7InjectChat(NextChatLineFn next, void *user, LogFn log,
                    YieldFn yieldUi, String *err);

} // namespace ldn
