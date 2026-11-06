/*
 * Civ VII LDN bootstrap flow for the CYD: AP teardown, advertisement scan +
 * decrypt (AES-CTR, SHA-256 verified), key derivation from /prod.keys on the
 * SD card, handshake-less association (ldn_hooks.cpp, AGPL-3.0), LDN
 * authentication (HMAC challenge), IP pickup from the host advertisement,
 * then FireCONN -> NetWelcome -> NetChat payload injection (ldn_civ7.cpp).
 *
 * SPDX-License-Identifier: GPL-3.0
 * Ported from SeanHeuc/CivSwitch esp32-ldn-civ7 (main/main.c, © Sean Heuc),
 * itself a port of CivSwitchLDN (GPL-3.0-only) and Kinnay's LDN (GPL-3.0);
 * proven on hardware by cyd-host/spike-ldn (ticket 09) on ESP-IDF v6.1.
 * This file targets the Arduino core's ESP-IDF v5.5.5 prebuilt wifi blobs -
 * the ABI it touches is restated in ldn_priv.h. The glue around the flow
 * (SD IO, logging, phase plumbing) carries the project license
 * (AGPL-3.0-only, see the repo-root LICENSE).
 *
 * Context rules: LogFn runs only in the calling task. Code that runs inside
 * the wifi rx callback (rx_cb, handleLdnFrame, civ7RxUdp, the installer
 * hook) only copies state and uses Serial.
 */
#include "ldn.h"

#include <SD.h>
#include <WiFi.h>
#include <esp_private/wifi.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <stdarg.h>
#include <stdio.h>

#include "../hw.h"
#include "../config.h"
#include "ldn_civ7.h"
#include "ldn_crypto.h"
#include "ldn_hooks.h"
#include "ldn_install.h"
#include "ldn_keys.h"
#include "ldn_priv.h"

namespace ldn {

namespace {

/* ---- Civ VII LDN protocol constants (CivSwitchLDN / Kinnay, public) ---- */
constexpr uint16_t ETH_TYPE_LDN = 0x88B7;
constexpr uint16_t INSTALLER_UDP_PORT = 9027;
constexpr uint16_t CIV7_UDP_PORT = 62056;
constexpr size_t FRAME_MAX = 1392;
constexpr const char *KEYS_PATH = "/prod.keys";
constexpr const char *CHAT_PATH =
    "/install/chat_payload.txt"; /* ticket 12 artifact */

const uint8_t SRC_ADV[16] = {0x19, 0x18, 0x84, 0x74, 0x3e, 0x24,
                             0xc7, 0x7d, 0x87, 0xc6, 0x9e, 0x42,
                             0x07, 0xd0, 0xc4, 0x38};
const uint8_t SRC_DATA[16] = {0xf1, 0xe7, 0x01, 0x84, 0x19, 0xa8,
                              0x4f, 0x71, 0x1d, 0xa7, 0x14, 0xc2,
                              0xcf, 0x91, 0x9c, 0x9c};
const uint8_t CHALLENGE_KEY[32] = {
    0xf8, 0x4b, 0x48, 0x7f, 0xb3, 0x72, 0x51, 0xc2, 0x63, 0xbf, 0x11, 0x60,
    0x90, 0x36, 0x58, 0x92, 0x66, 0xaf, 0x70, 0xca, 0x79, 0xb4, 0x4c, 0x93,
    0xc7, 0x37, 0x0c, 0x57, 0x69, 0xc0, 0xf6, 0x02};
/* CivSwitchLDN: bytes.fromhex("cb964487b449f4208b1eabee6e927f8947fad3d88c37") */
const uint8_t CIV7_PASS[22] = {0xcb, 0x96, 0x44, 0x87, 0xb4, 0x49, 0xf4,
                               0x20, 0x8b, 0x1e, 0xab, 0xee, 0x6e, 0x92,
                               0x7f, 0x89, 0x47, 0xfa, 0xd3, 0xd8, 0x8c,
                               0x37};

enum StaState { ST_IDLE, ST_SCANNING, ST_ASSOCIATING, ST_ASSOCIATED };

/* ---- state ---- */
LogFn s_log = nullptr;
PhaseFn s_phase = nullptr;
YieldFn s_yield = nullptr;
ProdKeys s_keys;

volatile StaState s_state = ST_IDLE;
volatile bool s_gotFrame;      /* scan: LDN advertisement captured */
volatile bool s_assocSeen;     /* assoc response seen */
volatile bool s_advFresh;      /* authed: fresh advertisement copied */
volatile bool s_advBusy;       /* task is parsing s_frame */
uint8_t s_frame[FRAME_MAX];
volatile int s_frameLen = 0;

uint8_t s_peer[6]; /* console MAC */
uint8_t s_staMac[6];
uint8_t s_ccmpKey[16];
char s_ssidHex[33];
uint8_t s_channel;

/* LDN network state */
uint64_t s_lcid;
uint16_t s_scene;
uint8_t s_ssid16[16];
uint8_t s_serverRandom[16];
uint64_t s_challenge;
uint16_t s_appVersion;
uint8_t s_clientRandom[16];
volatile bool s_authOk;
volatile uint8_t s_authStatus;
uint8_t s_localIp[4], s_hostIp[4];

void (*s_installerRx)(const uint8_t *data, size_t len) = nullptr;

/* ---- plumbing ---- */

void logf(const char *fmt, ...) {
  if (!s_log) return;
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  s_log(line);
}

void waitMs(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    if (s_yield) s_yield();
    delay(20);
  }
}

void put_u64le(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

/* ---- LDN auth frame rx (wifi callback context) ---- */

void handleLdnFrame(const uint8_t *data, size_t len) {
  if (len < 6) return;
  if (data[0] != 0x00 || data[1] != 0x22 || data[2] != 0xaa) return;
  uint16_t type = (data[3] << 8) | data[4];
  if (type != 0x0102) return; /* authentication frame */
  if (len < 6 + 8 + 32 + 16 + 16) return;
  const uint8_t *h = data + 6;
  if (!h[3]) return; /* is_response */
  s_authStatus = h[2];
  /* verify client_random echoes ours */
  if (memcmp(h + 8 + 32 + 16, s_clientRandom, 16) != 0) return;
  if (h[2] != 0) {
    Serial.printf("[ldn] auth status=%u\r\n", h[2]);
    return;
  }
  s_authOk = true;
}

/* ---- raw LDN ethernet tx ---- */

int ethSend(const uint8_t *payload, size_t len) {
  uint8_t frame[1600];
  if (len + 14 > sizeof(frame)) return -1;
  memcpy(frame, s_peer, 6);
  memcpy(frame + 6, s_staMac, 6);
  frame[12] = ETH_TYPE_LDN >> 8;
  frame[13] = ETH_TYPE_LDN & 0xff;
  memcpy(frame + 14, payload, len);
  return esp_wifi_internal_tx(WIFI_IF_STA, frame, (uint16_t)(14 + len));
}

/* ---- ethernet + UDP dispatch (wifi callback context) ---- */

esp_err_t ethernet_rx(void *buffer, uint16_t length, void *eb) {
  const uint8_t *f = (const uint8_t *)buffer;
  if (length >= 14) {
    uint16_t et = (f[12] << 8) | f[13];
    if (et == ETH_TYPE_LDN) {
      handleLdnFrame(f + 14, length - 14);
    } else if (et == 0x0800 && length >= 14 + 20 + 8) {
      const uint8_t *ip = f + 14;
      if ((ip[0] >> 4) == 4 && ip[9] == 17) {
        uint16_t ihl = (uint16_t)((ip[0] & 0x0f) * 4);
        if (ihl >= 20 && length >= 14 + ihl + 8) {
          const uint8_t *udp = ip + ihl;
          uint16_t dport = (uint16_t)((udp[2] << 8) | udp[3]);
          size_t dlen = length - 14 - ihl - 8;
          if (dport == CIV7_UDP_PORT) {
            civ7RxUdp(udp + 8, (uint16_t)dlen);
          } else if (dport == INSTALLER_UDP_PORT && s_installerRx) {
            s_installerRx(udp + 8, dlen);
          }
        }
      }
    }
  }
  esp_wifi_internal_free_rx_buffer(eb);
  return ESP_OK;
}

/* ---- promiscuous rx: advertisement capture + assoc response (wifi cb) ---- */

void rx_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t *p = (wifi_promiscuous_pkt_t *)buf;
  const uint8_t *f = (const uint8_t *)p->payload;
  int len = p->rx_ctrl.sig_len - 4; /* strip FCS */
  if (len < 24) return;
  uint8_t fc = f[0];
  if (((fc >> 2) & 0x3) != 0) return; /* mgmt only */
  uint8_t subtype = fc & 0xfc;

  /* assoc response while joining -> flag only; keys are installed from a
   * normal task context (calling auth_done from the wifi RX callback
   * crashes inside cnx_auth_done). */
  if (subtype == 0x10 && s_state == ST_ASSOCIATING && len >= 28 &&
      !memcmp(f + 4, s_staMac, 6) && !memcmp(f + 10, s_peer, 6) &&
      f[26] == 0 && f[27] == 0) {
    s_assocSeen = true;
    return;
  }

  /* LDN action frame (vendor OUI 00:22:aa): the advertisement carries the
   * encrypted lobby state; re-captured continuously while authed for the
   * IP lookup. */
  if (subtype == 0xd0 && len >= 28 && len <= (int)sizeof(s_frame) &&
      !memcmp(f + 24, "\x7f\x00\x22\xaa", 4)) {
    if (s_authOk && !s_advBusy) {
      memcpy(s_frame, f, len);
      s_frameLen = len;
      s_channel = p->rx_ctrl.channel;
      s_advFresh = true;
    } else if (s_state == ST_SCANNING && !s_gotFrame) {
      memcpy(s_frame, f, len);
      s_frameLen = len;
      s_channel = p->rx_ctrl.channel;
      s_gotFrame = true;
      Serial.printf("[ldn] captured LDN frame ch=%d rssi=%d len=%d\r\n",
                    p->rx_ctrl.channel, (int8_t)p->rx_ctrl.rssi, len);
    }
  }
}

/* ---- association ---- */

void staInstallKeys() {
  uint8_t seq[8] = {0};
  int pw = esp_wifi_set_sta_key_internal(WPA_ALG_CCMP, s_peer, 0, 1, seq, 8,
                                         s_ccmpKey, 16,
                                         KEY_FLAG_PAIRWISE | KEY_FLAG_RX |
                                             KEY_FLAG_TX);
  int grp = esp_wifi_set_sta_key_internal(WPA_ALG_CCMP, s_peer, 1, 0, seq, 8,
                                          s_ccmpKey, 16,
                                          KEY_FLAG_GROUP | KEY_FLAG_RX);
  if (pw || grp) {
    logf("key install failed pw=%d grp=%d", pw, grp);
    s_state = ST_IDLE;
    return;
  }
  esp_wifi_auth_done_internal();
  s_state = ST_ASSOCIATED;
  logf("ASSOCIATED to %02x:%02x:%02x:%02x:%02x:%02x (ch %d)", s_peer[0],
       s_peer[1], s_peer[2], s_peer[3], s_peer[4], s_peer[5], s_channel);
}

bool startAssociate(String *err) {
  for (int attempt = 1; attempt <= 5 && s_state != ST_ASSOCIATED; attempt++) {
    logf("assoc attempt %d/5", attempt);
    esp_wifi_stop();
    esp_wifi_set_mac(WIFI_IF_STA, s_staMac);
    esp_wifi_set_protocol(WIFI_IF_STA,
                          WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                              WIFI_PROTOCOL_11N);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    /* keep promiscuous on so rx_cb can catch the association response */
    wifi_promiscuous_filter_t flt = {.filter_mask =
                                         WIFI_PROMIS_FILTER_MASK_MGMT |
                                         WIFI_PROMIS_FILTER_MASK_DATA};
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(&rx_cb);
    esp_wifi_set_promiscuous(true);

    wifi_config_t cfg = {};
    memcpy(cfg.sta.ssid, s_ssidHex, 32);
    memcpy(cfg.sta.password, "00000000", 8);
    memcpy(cfg.sta.bssid, s_peer, 6);
    cfg.sta.bssid_set = true;
    cfg.sta.channel = s_channel;
    cfg.sta.scan_method = WIFI_FAST_SCAN;
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);

    logf("associating: ssid=%s ch=%d mac=%02x:%02x:%02x:%02x:%02x:%02x",
         s_ssidHex, s_channel, s_staMac[0], s_staMac[1], s_staMac[2],
         s_staMac[3], s_staMac[4], s_staMac[5]);
    s_state = ST_ASSOCIATING;
    s_assocSeen = false;
    esp_err_t r = esp_wifi_connect();
    if (r != ESP_OK) {
      waitMs(500);
      continue;
    }
    for (int i = 0; i < 60 && s_state == ST_ASSOCIATING; i++) {
      if (s_assocSeen && esp_wifi_sta_is_running_internal()) {
        staInstallKeys(); /* normal task context */
        break;
      }
      waitMs(50);
    }
    if (s_state == ST_ASSOCIATED) break;
    logf("attempt %d did not associate", attempt);
    esp_wifi_disconnect();
    waitMs(300);
  }
  if (s_state != ST_ASSOCIATED) {
    if (err) *err = "all association attempts failed";
    return false;
  }
  return true;
}

/* ---- LDN authentication (HMAC challenge) ---- */

void ldnAuthenticate() {
  esp_wifi_internal_reg_rxcb(WIFI_IF_STA, ethernet_rx);

  /* ChallengeRequest body (0x2D0) */
  static uint8_t body[0x2D0];
  memset(body, 0, sizeof(body));
  put_u64le(body + 8, s_challenge);
  uint64_t r1 = ((uint64_t)esp_random() << 32) | esp_random();
  uint64_t r2 = ((uint64_t)esp_random() << 32) | esp_random();
  put_u64le(body + 16, r1);
  put_u64le(body + 24, r2);
  uint8_t mac[32] = {0};
  int hst = crypto::hmacSha256(CHALLENGE_KEY, sizeof(CHALLENGE_KEY), body,
                               sizeof(body), mac);
  logf("challenge hmac -> %d", hst);

  static uint8_t ch[0x300];
  memset(ch, 0, sizeof(ch));
  memcpy(ch + 4, mac, 32);
  memcpy(ch + 4 + 32 + 12, body, sizeof(body));

  /* AuthenticationRequest payload */
  static uint8_t req[100 + 0x300];
  memset(req, 0, sizeof(req));
  const char *name = "Civ7-ESP32";
  memcpy(req, name, strlen(name));
  req[32] = s_appVersion >> 8; /* u16 BE */
  req[33] = s_appVersion & 0xff;
  req[34] = 0; /* platform NX */
  memcpy(req + 100, ch, sizeof(ch));

  /* AuthenticationFrame (protocol 1 -> plaintext) */
  uint16_t size = sizeof(req);
  static uint8_t frame[8 + 32 + 16 + 16 + 0x300 + 0x100];
  uint8_t *o = frame;
  *o++ = 0x00;
  *o++ = 0x22;
  *o++ = 0xaa;              /* OUI */
  *o++ = 0x01;              /* auth frame */
  *o++ = 0x02;
  *o++ = 0x00;              /* pad */
  *o++ = 4;                 /* version */
  *o++ = size & 0xff;       /* size lo */
  *o++ = 0;                 /* status */
  *o++ = 0;                 /* is_response = 0 */
  *o++ = size >> 8;         /* size hi */
  *o++ = 0;                 /* AUTH_FORMAT_PLAIN */
  *o++ = 0;
  *o++ = 0;                 /* pad 2 */
  /* network_id, little-endian */
  put_u64le(o, s_lcid);
  o += 8;
  *o++ = 0;
  *o++ = 0;
  o[0] = s_scene & 0xff;
  o[1] = s_scene >> 8;
  o += 2;
  memset(o, 0, 4);
  o += 4;
  memcpy(o, s_ssid16, 16);
  o += 16;
  memcpy(o, s_serverRandom, 16);
  o += 16;
  memcpy(o, s_clientRandom, 16);
  o += 16;
  memcpy(o, req, sizeof(req));
  o += sizeof(req);

  size_t flen = (size_t)(o - frame);
  logf("auth request: %u bytes lcid=%016llx scene=%04x", (unsigned)flen,
       (unsigned long long)s_lcid, s_scene);
  for (int attempt = 1; attempt <= 3 && !s_authOk; attempt++) {
    int tx = ethSend(frame, flen);
    logf("auth tx -> %d", tx);
    waitMs(700); /* spike: 14 x 50 ms */
    logf("auth attempt %d -> %s status=%u", attempt,
         s_authOk ? "OK" : "retry", s_authStatus);
  }
}

/* ---- advertisement parse / decrypt (task context) ---- */

bool decryptAdvertisement(String *err) {
  const uint8_t *f = s_frame;
  int len = s_frameLen;
  if (len < 24 + 12 + 40) {
    if (err) *err = "advertisement frame too short";
    return false;
  }

  memcpy(s_peer, f + 10, 6); /* addr2 = console MAC */

  const uint8_t *network_id = f + 36; /* 32 bytes */
  uint8_t advertiseKey[16];
  crypto::deriveKey(s_keys, network_id, 32, SRC_ADV, advertiseKey);

  uint8_t format = f[69]; /* header: version@68, format@69 */
  uint16_t size = (uint16_t)((f[70] << 8) | f[71]);
  const uint8_t *nonce = f + 72; /* 4 bytes */
  logf("format=%d size=0x%x", format, size);
  /* The Civ VII advertisement payload is 0x500 bytes; the auth challenge
   * lives at payload offset 1272, so anything shorter is not a lobby adv. */
  if (size < 1280) {
    if (err) *err = "advertisement payload too small";
    return false;
  }
  if (size > 0x500) {
    if (err) *err = "advertisement payload size out of range";
    return false;
  }

  memcpy(s_ssid16, network_id + 16, 16); /* ssid at network_id[16..31] */
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    s_ssidHex[2 * i] = hex[s_ssid16[i] >> 4];
    s_ssidHex[2 * i + 1] = hex[s_ssid16[i] & 15];
  }
  s_ssidHex[32] = 0;
  logf("ssid=%s", s_ssidHex);

  /* fields the authentication frame needs */
  s_lcid = 0;
  for (int i = 0; i < 8; i++)
    s_lcid = (s_lcid << 8) | network_id[i]; /* BE u64 */
  s_scene = (uint16_t)((network_id[10] << 8) | network_id[11]); /* BE u16 */
  esp_fill_random(s_clientRandom, 16);

  /* body ciphertext: 32-byte sha + size bytes, encrypted AES-CTR */
  int ct_len = 32 + size;
  uint8_t *plain = (uint8_t *)malloc(ct_len);
  if (!plain) {
    if (err) *err = "out of heap for advertisement decrypt";
    return false;
  }
  memcpy(plain, f + 76, ct_len);
  crypto::aesCtr(advertiseKey, nonce, plain, ct_len);

  /* verify: sha256(header + 32 zero bytes + payload) == plain[0..31] */
  static uint8_t tmp[40 + 32 + 0x500];
  memcpy(tmp, f + 36, 40);
  memset(tmp + 40, 0, 32);
  memcpy(tmp + 72, plain + 32, size);
  uint8_t dig[32];
  crypto::sha256(tmp, 40 + 32 + size, dig);
  bool shaOk = memcmp(dig, plain, 32) == 0;
  logf("adv sha %s", shaOk ? "OK" : "MISMATCH");
  if (!shaOk) {
    free(plain);
    if (err) *err = "advertisement sha mismatch (wrong prod.keys?)";
    return false;
  }

  memcpy(s_serverRandom, plain + 32, 16);
  /* challenge u64 BE lives at payload offset 1272 (0x500 payload) */
  s_challenge = 0;
  const uint8_t *chp = plain + 32 + 1272;
  for (int i = 0; i < 8; i++) s_challenge = (s_challenge << 8) | chp[i];
  s_appVersion = (uint16_t)((plain[32 + 68] << 8) | plain[32 + 69]);
  logf("app_version=%u", s_appVersion);
  free(plain);

  /* data key (CCMP) = KDF(server_random + Civ VII passphrase) */
  uint8_t data[16 + sizeof(CIV7_PASS)];
  memcpy(data, s_serverRandom, 16);
  memcpy(data + 16, CIV7_PASS, sizeof(CIV7_PASS));
  crypto::deriveKey(s_keys, data, sizeof(data), SRC_DATA, s_ccmpKey);
  logf("ccmp key derived");
  return true;
}

/* Parse the latest advertisement and find our participant entry (IP). */
bool advFindIp() {
  const uint8_t *f = s_frame;
  int len = s_frameLen;
  if (len < 24 + 12 + 40) return false;
  const uint8_t *network_id = f + 36;
  uint8_t advKey[16];
  crypto::deriveKey(s_keys, network_id, 32, SRC_ADV, advKey);
  if (f[69] != 2) return false;
  uint16_t size = (uint16_t)((f[70] << 8) | f[71]);
  int ct_len = 32 + size;
  if (size == 0 || 76 + ct_len > len) return false;
  uint8_t *plain = (uint8_t *)malloc(ct_len);
  if (!plain) return false;
  memcpy(plain, f + 76, ct_len);
  crypto::aesCtr(advKey, f + 72, plain, ct_len);
  const uint8_t *p = plain + 32;
  memcpy(s_hostIp, p + 24, 4); /* participants[0].ip */
  bool found = false;
  for (int i = 0; i < 8; i++) {
    const uint8_t *pt = p + 24 + i * 56;
    if (pt[10] && memcmp(pt + 4, s_staMac, 6) == 0) {
      memcpy(s_localIp, pt, 4);
      found = true;
      logf("our IP from advertisement slot %d: %u.%u.%u.%u", i, pt[0], pt[1],
           pt[2], pt[3]);
      break;
    }
  }
  free(plain);
  if (!found) return false;
  logf("host IP: %u.%u.%u.%u", s_hostIp[0], s_hostIp[1], s_hostIp[2],
       s_hostIp[3]);
  return true;
}

/* ---- chat payload streaming from the SD card ---- */

File s_chatFile;

int nextChatLine(int index, char *buf, size_t cap, void *user) {
  (void)index;
  (void)user;
  if (!s_chatFile) return 0;
  hw::busForSd();
  int n = 0;
  bool got = false;
  for (;;) {
    if (!s_chatFile.available()) break;
    int c = s_chatFile.read();
    if (c < 0) break;
    if (c == '\n') {
      if (got) break; /* real line done; skip empty lines */
      continue;
    }
    if (c != '\r') {
      if (n < (int)cap - 1) buf[n++] = (char)c;
      got = true;
    }
  }
  return got ? n : 0;
}

int countChatLines() {
  hw::busForSd();
  int lines = 0;
  bool got = false;
  while (s_chatFile.available()) {
    int c = s_chatFile.read();
    if (c == '\n') {
      if (got) lines++;
      got = false;
    } else if (c != '\r') {
      got = true;
    }
  }
  if (got) lines++;
  s_chatFile.seek(0);
  return lines;
}

/* ---- radio ---- */

bool radioStart() {
  /* Drop the AP; the CYD reboots back into AP mode after the run. */
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  waitMs(150);

  /* WiFi.mode(WIFI_OFF) does not just stop the radio - the Arduino core
   * (wifiLowLevelDeinit) runs esp_wifi_deinit() and destroys its netifs.
   * Bring the driver back exactly like the spike's radio_init: raw driver,
   * no netif, RAM storage (we only use internal TX/RX + the rx callback). */
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) return false;
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  /* A fresh driver reset the country; re-pin it so channels 1-11 stay open
   * (same call net::apStart makes). */
  esp_wifi_set_country_code(WIFI_COUNTRY, true);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_fill_random(s_staMac, 6);
  s_staMac[0] = (s_staMac[0] & 0xfc) | 2; /* fixed, locally administered */
  esp_wifi_set_mac(WIFI_IF_STA, s_staMac);
  esp_wifi_set_protocol(WIFI_IF_STA,
                        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                            WIFI_PROTOCOL_11N);
  esp_wifi_start();
  esp_wifi_set_ps(WIFI_PS_NONE);
  wifi_promiscuous_filter_t flt = {.filter_mask =
                                       WIFI_PROMIS_FILTER_MASK_MGMT |
                                       WIFI_PROMIS_FILTER_MASK_DATA};
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(&rx_cb);
  esp_wifi_set_promiscuous(true);
  hooks::install();
  logf("radio ready, sta mac %02x:%02x:%02x:%02x:%02x:%02x", s_staMac[0],
       s_staMac[1], s_staMac[2], s_staMac[3], s_staMac[4], s_staMac[5]);
  return true;
}

bool scanAdv(String *err) {
  static const uint8_t chans[] = {1, 6, 11};
  constexpr uint32_t DWELL_MS = 600;
  s_gotFrame = false;
  s_state = ST_SCANNING;
  for (int round = 0; round < 6 && !s_gotFrame; round++) {
    for (size_t i = 0; i < sizeof(chans) && !s_gotFrame; i++) {
      esp_wifi_set_channel(chans[i], WIFI_SECOND_CHAN_NONE);
      waitMs(DWELL_MS);
    }
  }
  if (!s_gotFrame) {
    if (err) *err = "no LDN advertisement found - is the lobby open?";
    s_state = ST_IDLE;
    return false;
  }
  s_channel = s_channel ? s_channel : 1;
  return true;
}

void resetState() {
  s_state = ST_IDLE;
  s_gotFrame = false;
  s_assocSeen = false;
  s_advFresh = false;
  s_advBusy = false;
  s_frameLen = 0;
  memset(s_peer, 0, sizeof(s_peer));
  memset(s_ccmpKey, 0, sizeof(s_ccmpKey));
  memset(s_ssid16, 0, sizeof(s_ssid16));
  memset(s_serverRandom, 0, sizeof(s_serverRandom));
  memset(s_clientRandom, 0, sizeof(s_clientRandom));
  memset(s_localIp, 0, sizeof(s_localIp));
  memset(s_hostIp, 0, sizeof(s_hostIp));
  s_lcid = 0;
  s_scene = 0;
  s_challenge = 0;
  s_appVersion = 0;
  s_authOk = false;
  s_authStatus = 0;
  s_channel = 0;
}

} // namespace

/* ------------------------------------------------------- public API */

bool bootstrap(LogFn log, PhaseFn phase, YieldFn yieldUi, ProgressFn progress,
               String *err) {
  s_log = log;
  s_phase = phase;
  s_yield = yieldUi;
  if (err) *err = "";
  resetState();
  if (s_chatFile) s_chatFile.close();

  if (s_phase) s_phase("keys");
  if (!loadProdKeys(KEYS_PATH, &s_keys, err)) return false;
  logf("prod.keys loaded");

  if (s_phase) s_phase("radio");
  if (!radioStart()) {
    if (err) *err = "esp_wifi_init failed after AP teardown";
    return false;
  }

  if (s_phase) s_phase("scan");
  if (!scanAdv(err)) return false;

  if (s_phase) s_phase("decrypt");
  if (!decryptAdvertisement(err)) return false;

  if (s_phase) s_phase("associate");
  if (!startAssociate(err)) return false;

  if (s_phase) s_phase("auth");
  ldnAuthenticate();
  if (!s_authOk) {
    char ebuf[64];
    snprintf(ebuf, sizeof(ebuf), "LDN authentication failed (status %u)",
             s_authStatus);
    if (err) *err = ebuf;
    return false;
  }
  logf("LDN AUTHENTICATED");
  if (s_phase) s_phase("ip");
  bool haveIp = false;
  for (int i = 0; i < 150 && !haveIp; i++) { /* ~15 s */
    if (s_advFresh) {
      s_advFresh = false;
      s_advBusy = true;
      haveIp = advFindIp();
      s_advBusy = false;
    }
    if (!haveIp) waitMs(100);
  }
  if (!haveIp) {
    if (err) *err = "no IP in the host advertisement (our slot not handed out)";
    return false;
  }

  if (s_phase) s_phase("chat");
  civ7SetNet(s_localIp, s_hostIp, s_peer, s_staMac);

  hw::busForSd();
  s_chatFile = SD.open(CHAT_PATH, FILE_READ);
  if (!s_chatFile) {
    if (err)
      *err = String("missing ") + CHAT_PATH +
             " - build the bundle with build_sd.py --ldn (ticket 12)";
    return false;
  }
  int lines = countChatLines();
  logf("chat payload: %d lines", lines);

  if (!civ7Handshake(log, yieldUi, err)) {
    s_chatFile.close();
    return false;
  }
  if (!civ7InjectChat(nextChatLine, nullptr, log, yieldUi, err)) {
    s_chatFile.close();
    return false;
  }
  s_chatFile.close();

  /* the console now runs the payload and binds UDP :9027; stream the
   * installer (template + apply JS) from the SD bundle (ticket 11) */
  if (s_phase) s_phase("installer");
  if (!installerRun(log, yieldUi, progress, err)) return false;
  return true;
}

void setInstallerRx(void (*fn)(const uint8_t *data, size_t len)) {
  s_installerRx = fn;
}

int udpSend(uint16_t port, const uint8_t *data, size_t len) {
  return civ7UdpSend(port, data, len);
}

const uint8_t *localIp() { return s_localIp; }
const uint8_t *hostIp() { return s_hostIp; }
const uint8_t *hostMac() { return s_peer; }

} // namespace ldn
