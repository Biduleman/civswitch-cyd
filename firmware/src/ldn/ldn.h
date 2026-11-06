/**
 * CivSwitch CYD host - LDN bootstrap module (ticket 10, ticket 11).
 *
 * Joins the Civ VII wireless lobby's LDN network from a running AP-mode
 * state, injects the CivSwitch payload as chat messages, then streams the
 * savefile installer over UDP :9027:
 *
 *   scan -> advertisement decrypt -> key derivation (from /prod.keys on the
 *   SD card, derived on-device - no build-time key baking) -> handshake-less
 *   association -> LDN authentication (HMAC challenge) -> FireCONN ->
 *   NetWelcome -> NetChat injection -> installer chunks (UDP :9027).
 *
 * Ported from SeanHeuc/CivSwitch esp32-ldn-civ7 (main/main.c, main/civ7.c,
 * main/installer.c UDP path) and hardware-proven in the spike
 * (cyd-host/spike-ldn, ticket 09) on ESP-IDF v6.1. This module targets the
 * Arduino core's ESP-IDF v5.5.5 prebuilt wifi blobs; ldn_priv.h restates
 * that ABI (verified against the installed framework libs and on hardware).
 *
 * License: this module is part of the CivSwitch workspace, licensed
 * AGPL-3.0-only (see the LICENSE file at the repo root). Ported components
 * keep their upstream licenses and attribution, per file:
 *   ldn.h, ldn_keys.*                 glue (this repo)
 *   ldn.cpp                           esp32-ldn-civ7 © Sean Heuc (main.c;
 *                                     CivSwitchLDN [GPL-3.0-only] /
 *                                      Kinnay LDN [GPL-3.0] lineage)
 *   ldn_civ7.*                        esp32-ldn-civ7 © Sean Heuc (civ7.c;
 *                                     CivSwitchLDN, GPL-3.0-only lineage)
 *   ldn_crypto.*                      Kinnay LDN key derivation (GPL-3.0)
 *   ldn_install.*                     esp32-ldn-civ7 © Sean Heuc
 *                                     (installer.c, UDP path)
 *   ldn_priv.h, ldn_hooks.*           pokeldn wpa_funcs patch (AGPL-3.0)
 * Full table + credits: firmware/README.md and the root README.md.
 */
#pragma once

#include <Arduino.h>

namespace ldn {

typedef void (*LogFn)(const char *line);
typedef void (*PhaseFn)(const char *phase);
/** Pump the UI (LVGL + touch). Called from the task running bootstrap. */
typedef void (*YieldFn)(void);
/** Installer chunk progress; (0,0) clears. May be nullptr. */
typedef void (*ProgressFn)(int done, int total);

/**
 * Runs the full LDN bootstrap: leaves AP mode, joins the lobby, injects the
 * chat payload (/install/chat_payload.txt) and streams the installer
 * (install/tpl_b64.txt + install/apply.js) over UDP :9027. Blocks (calls
 * yieldUi every ~20 ms while waiting) until done or failure. The AP is not
 * restored either way - the caller reboots the CYD (ESP.restart()) so the
 * AP comes back (ticket 11 button flow does this).
 * Reads from the SD card: /prod.keys, /install/chat_payload.txt,
 * /install/tpl_b64.txt, /install/apply.js.
 * Phases reported via phase(): "keys", "radio", "scan", "decrypt",
 * "associate", "auth", "ip", "chat", "installer".
 */
bool bootstrap(LogFn log, PhaseFn phase, YieldFn yieldUi, ProgressFn progress,
               String *err);

/** Register a receiver for UDP datagrams the console sends to :9027
 * (ticket 11's installer channel). Called from the wifi rx callback: must
 * not log via LVGL/SD, only copy state. nullptr unregisters. */
void setInstallerRx(void (*fn)(const uint8_t *data, size_t len));

/** Send a raw UDP datagram to the console over the LDN link. */
int udpSend(uint16_t port, const uint8_t *data, size_t len);

/* Network state, valid after bootstrap() passed the given phase. */
const uint8_t *localIp();  /* after "ip"   */
const uint8_t *hostIp();   /* after "ip"   */
const uint8_t *hostMac();  /* after "scan" */

} // namespace ldn
