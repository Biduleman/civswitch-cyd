/**
 * Installer streaming over the LDN link (UDP :9027).
 *
 * After the chat payload brought the Remote JS Loader up, the console binds
 * a UDP receiver on :9027 (the udp_loader.js splice). This module streams
 * the install blobs - install/tpl_b64.txt (base64 save template) and
 * install/apply.js (base64-encoded on the fly) - from the SD card in
 * numbered chunks, with a NAK repair window. The console assembles both
 * blobs, runs the apply JS (writes the boot-persistent trigger) and
 * switches to the TCP loader on :9026 on its own; the CYD just reboots
 * back into AP mode afterwards.
 *
 * Wire format (ported verbatim from SeanHeuc's esp32-ldn-civ7
 * main/installer.c, © Sean Heuc):
 *   board -> console: "A<ntpl>:<njs>:<board-ip>", "T<i>:<b64>", "J<i>:<b64>", "G"
 *   console -> board: "D" (all received), "t<i>"/"j<i>" (resend that chunk)
 *
 * License: SPDX-License-Identifier: GPL-3.0 (port of installer.c from the
 * CivSwitchLDN lineage - see ldn.h licensing). The SD/progress glue around
 * it is trivial and carries the project license (AGPL-3.0-only).
 * Runs in the calling task only; the :9027 rx hook is registered via
 * ldn::setInstallerRx() for the duration of the stream.
 * Ticket 11.
 */
#pragma once

#include <Arduino.h>

#include "ldn.h"

namespace ldn {

/** progress(done, total) - installer chunks (0,0 clears). May be nullptr. */
typedef void (*ProgressFn)(int done, int total);

/**
 * Streams the installer from the SD bundle over UDP :9027. Must be called
 * after bootstrap() succeeded (LDN link up, chat payload running on the
 * console). Blocks up to ~15 s (fast pass + repair window).
 * Files: /install/tpl_b64.txt, /install/apply.js (bundle:: paths).
 */
bool installerRun(LogFn log, YieldFn yieldUi, ProgressFn progress, String *err);

} // namespace ldn
