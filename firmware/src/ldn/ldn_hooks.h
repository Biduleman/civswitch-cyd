/*
 * wpa_funcs overrides for handshake-less LDN association (pokeldn's
 * approach, restated for the Arduino core's ESP-IDF v5.5.5 blobs).
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 * Upstream: https://github.com/Decryptu/pokeldn (AGPL-3.0) - see ldn_priv.h.
 * The RSN-IE + connect hook makes the station join the console's hidden LDN
 * network without a 4-way handshake; ldn.cpp installs the CCMP keys
 * manually afterwards.
 */
#pragma once

namespace ldn {
namespace hooks {

/** Swaps wpa_cb for a patched table (station side only). Call once after
 * the radio is started in STA mode and before esp_wifi_connect(). */
void install();

} // namespace hooks
} // namespace ldn
