/*
 * LDN association hooks - pokeldn's wpa_funcs patch (AGPL-3.0,
 * https://github.com/Decryptu/pokeldn), ported through SeanHeuc's
 * esp32-ldn-civ7 (main/main.c install_hooks) and the spike
 * (cyd-host/spike-ldn).
 * SPDX-License-Identifier: AGPL-3.0-only - see ldn_priv.h.
 *
 * Runs partly in the wifi driver's task context (the connect hook fires
 * from inside esp_wifi_connect()), so logging here goes to Serial only.
 */
#include "ldn_hooks.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "ldn_priv.h"

namespace ldn {
namespace hooks {

/* CivSwitchLDN's RSN IE: CCMP pairwise/group + GCMP-128 management. */
static uint8_t s_rsn_ie[] = {
    0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
    0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x0c, 0x00,
};
static int (*s_stock_sta_connect)(uint8_t *bssid);

static int ldn_sta_connect(uint8_t *bssid) {
  int r = esp_wifi_set_appie_internal(APPIE_RSN, s_rsn_ie, sizeof(s_rsn_ie), 0);
  if (r == 0 && s_stock_sta_connect) r = s_stock_sta_connect(bssid);
  if (r == 0) r = esp_wifi_set_appie_internal(APPIE_RSN, s_rsn_ie, sizeof(s_rsn_ie), 0);
  Serial.printf("[ldn] sta_connect hook fired (appie r=%d)\r\n", r);
  return r;
}
static int ldn_sta_rx_eapol(uint8_t *src, uint8_t *buf, uint32_t len) {
  (void)src; (void)buf; (void)len;
  return 0;
}
static bool ldn_sta_in_handshake(void) { return false; }
static void ldn_sta_connected(uint8_t *bssid) { (void)bssid; }

void install() {
  if (!wpa_cb) {
    Serial.println("[ldn] hooks: wpa_cb missing - abort");
    return;
  }
  struct wpa_funcs *t = (struct wpa_funcs *)malloc(sizeof(*t));
  if (!t) return;
  memcpy(t, wpa_cb, sizeof(*t));
  s_stock_sta_connect = t->wpa_sta_connect;
  t->wpa_sta_connect = ldn_sta_connect;
  t->wpa_sta_connected_cb = ldn_sta_connected;
  t->wpa_sta_rx_eapol = ldn_sta_rx_eapol;
  t->wpa_sta_in_4way_handshake = ldn_sta_in_handshake;
  esp_wifi_register_wpa_cb_internal(t);
  wpa_cb = t; /* esp_wpa_main.c owns this pointer; register() already set it */
}

} // namespace hooks
} // namespace ldn
