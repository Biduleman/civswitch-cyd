/*
 * Civ VII LDN game protocol, ported from CivSwitchLDN's
 * ldn/{protocol,transport,lobby}.py (GPL-3.0-only) via SeanHeuc's
 * esp32-ldn-civ7 (main/civ7.c, © Sean Heuc).
 * SPDX-License-Identifier: GPL-3.0
 * Ticket 10.
 *
 * Adaptations from upstream: vTaskDelay -> delay(), esp_timer -> millis(),
 * chat lines stream in through a callback instead of the build-time
 * chat_payload.h, logging goes through LogFn (task context only - the rx
 * path stays silent so it can run inside the wifi callback).
 */
#include "ldn_civ7.h"

#include <Arduino.h>
#include <esp_private/wifi.h>
#include <esp_wifi.h>
#include <stdlib.h>
#include <string.h>

namespace ldn {

constexpr uint16_t CIV7_PORT = 62056;
constexpr size_t TX_RING = 64;

/* Upstream keeps the net state as file statics; we mirror that layout. */
static uint8_t s_localIp[4], s_hostIp[4], s_hostMac[6], s_staMac[6];
static uint64_t s_nextSeq = 1; /* FireCONN uses sequence 0 */
static uint32_t s_counter;     /* message counter = packets queued before */
static volatile bool s_welcome;
static uint32_t s_slot = 1;

static uint8_t *s_tx[TX_RING];
static uint16_t s_tx_len[TX_RING];
static uint64_t s_tx_seq[TX_RING];

static void ip_reversed(const uint8_t ip[4], uint8_t out[4]) {
  out[0] = ip[3];
  out[1] = ip[2];
  out[2] = ip[1];
  out[3] = ip[0];
}

/* ------------------------------------------------------------------ UDP/IP */

static uint32_t sum16(const uint8_t *d, size_t n) {
  uint32_t sum = 0;
  size_t i = 0;
  for (; i + 1 < n; i += 2) sum += ((uint32_t)d[i] << 8) | d[i + 1];
  if (i < n) sum += (uint32_t)d[i] << 8;
  return sum;
}

static uint16_t ip_checksum(const uint8_t *d, size_t n) {
  uint32_t sum = sum16(d, n);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return (uint16_t)~sum;
}

static int udp_send_port(uint16_t dport, const uint8_t *payload, size_t len) {
  static uint8_t frame[1600];
  size_t udp_len = 8 + len;
  size_t ip_len = 20 + udp_len;
  if (14 + ip_len > sizeof(frame)) return -1;

  memcpy(frame, s_hostMac, 6);
  memcpy(frame + 6, s_staMac, 6);
  frame[12] = 0x08;
  frame[13] = 0x00;

  uint8_t *ip = frame + 14;
  memset(ip, 0, 20);
  ip[0] = 0x45;
  ip[2] = (ip_len >> 8) & 0xff;
  ip[3] = ip_len & 0xff;
  ip[8] = 64;
  ip[9] = 17;
  memcpy(ip + 12, s_localIp, 4);
  memcpy(ip + 16, s_hostIp, 4);
  uint16_t ck = ip_checksum(ip, 20);
  ip[10] = ck >> 8;
  ip[11] = ck & 0xff;

  uint8_t *udp = ip + 20;
  udp[0] = dport >> 8;
  udp[1] = dport & 0xff;
  udp[2] = dport >> 8;
  udp[3] = dport & 0xff;
  udp[4] = (udp_len >> 8) & 0xff;
  udp[5] = udp_len & 0xff;
  udp[6] = 0;
  udp[7] = 0;
  memcpy(udp + 8, payload, len);

  /* UDP checksum over pseudo header + UDP */
  uint32_t sum = sum16(s_localIp, 4) + sum16(s_hostIp, 4);
  sum += 17;
  sum += (uint32_t)udp_len;
  sum += sum16(udp, udp_len);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  uint16_t uck = (uint16_t)~sum;
  if (uck == 0) uck = 0xffff;
  udp[6] = uck >> 8;
  udp[7] = uck & 0xff;

  return esp_wifi_internal_tx(WIFI_IF_STA, frame, (uint16_t)(14 + ip_len));
}

static int udp_send(const uint8_t *payload, size_t len) {
  return udp_send_port(CIV7_PORT, payload, len);
}

int civ7UdpSend(uint16_t port, const uint8_t *data, size_t len) {
  return udp_send_port(port, data, len);
}

/* --------------------------------------------------------------- transport */

static void store(uint64_t seq, const uint8_t *pkt, size_t len) {
  int i = (int)(seq % TX_RING);
  free(s_tx[i]);
  s_tx[i] = (uint8_t *)malloc(len);
  if (!s_tx[i]) return;
  memcpy(s_tx[i], pkt, len);
  s_tx_len[i] = len;
  s_tx_seq[i] = seq;
}

static void resend(uint64_t seq) {
  int i = (int)(seq % TX_RING);
  if (s_tx[i] && s_tx_seq[i] == seq) udp_send(s_tx[i], s_tx_len[i]);
}

/* packet = reversed(dst ip) + kind + body */
static uint64_t transport_send(const uint8_t *body, size_t len, bool initial) {
  uint8_t *pkt = (uint8_t *)malloc(5 + len);
  if (!pkt) return 0;
  ip_reversed(s_hostIp, pkt);
  pkt[4] = 'F';
  memcpy(pkt + 5, body, len);
  uint64_t seq = initial ? 0 : s_nextSeq;
  if (!initial) s_nextSeq++;
  udp_send(pkt, 5 + len);
  store(seq, pkt, 5 + len);
  free(pkt);
  return seq;
}

static void send_control(char kind, uint64_t seq) {
  uint8_t pkt[13];
  ip_reversed(s_hostIp, pkt);
  pkt[4] = (uint8_t)kind;
  for (int i = 0; i < 8; i++) pkt[5 + i] = (uint8_t)(seq >> (8 * i)) & 0xff;
  udp_send(pkt, sizeof(pkt));
}

static void queue_msg(const uint8_t *payload, size_t len) {
  uint8_t *body = (uint8_t *)malloc(8 + len);
  if (!body) return;
  uint64_t seq = s_nextSeq;
  for (int i = 0; i < 8; i++) body[i] = (uint8_t)(seq >> (8 * i)) & 0xff;
  memcpy(body + 8, payload, len);
  transport_send(body, 8 + len, false);
  free(body);
}

/* ------------------------------------------------------------ rx (wifi cb) */

void civ7RxUdp(const uint8_t *data, uint16_t len) {
  if (len < 5) return;
  uint8_t want[4];
  ip_reversed(s_localIp, want);
  if (memcmp(data, want, 4) != 0) return; /* destination prefix */
  char kind = (char)data[4];
  const uint8_t *body = data + 5;
  size_t blen = len - 5;

  if (kind == 'S') {
    if (blen == 1 && body[0] == 'P') send_control('S', 0); /* pong */
    return;
  }
  if (kind == 'R' && blen >= 8) {
    uint64_t seq = 0;
    for (int i = 0; i < 8; i++) seq |= (uint64_t)body[i] << (8 * i);
    resend(seq);
    return;
  }
  if (kind == 'V' && blen >= 8) {
    if (s_nextSeq > 0) resend(s_nextSeq - 1);
    return;
  }
  if (kind != 'F' || blen < 8) return;

  const uint8_t *msg = body + 8;
  size_t mlen = blen - 8;

  static const uint8_t REPLY[8] = {0x59, 0x4c, 0x50, 0x52,
                                   0x65, 0x72, 0x69, 0x46}; /* FireRPLY */
  if (mlen >= 8 && memcmp(msg, REPLY, 8) == 0) {
    s_welcome = true;
    if (mlen >= 44)
      s_slot = msg[40] | (msg[41] << 8) | (msg[42] << 16) |
               ((uint32_t)msg[43] << 24);
    return;
  }
  if (mlen >= 28) {
    uint32_t type = msg[0] | (msg[1] << 8) | (msg[2] << 16) |
                    ((uint32_t)msg[3] << 24);
    if (type == 0x4B0) { /* NetWelcome */
      s_welcome = true;
      if (mlen >= 44)
        s_slot = msg[40] | (msg[41] << 8) | (msg[42] << 16) |
                 ((uint32_t)msg[43] << 24);
    }
  }
}

/* --------------------------------------------------------------- messages */

static void put_u32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static size_t message_header(uint8_t *out, uint32_t kind, uint32_t counter) {
  uint32_t h[7] = {kind, 1, 1, 0xFFFFFFFF, 0xFFFFFFFF, 1, counter};
  for (int i = 0; i < 7; i++) put_u32le(out + 4 * i, h[i]);
  return 28;
}

static void send_net_register() {
  uint8_t b[28 + 4 + 5];
  size_t n = message_header(b, 0x4B8, s_counter++);
  put_u32le(b + n, 5);
  n += 4;
  memcpy(b + n, "1.0.0", 5);
  n += 5;
  queue_msg(b, n);
}

static void send_net_app_register() {
  uint8_t b[28 + 4 + 1];
  size_t n = message_header(b, 0x4B9, s_counter++);
  put_u32le(b + n, s_slot);
  n += 4;
  b[n++] = 0;
  queue_msg(b, n);
}

static void send_net_snapshot() {
  uint8_t b[28 + 4 + 1];
  size_t n = message_header(b, 0x4E9, s_counter++);
  put_u32le(b + n, 0);
  n += 4;
  b[n++] = 1;
  queue_msg(b, n);
}

static void send_net_chat(const char *text, size_t tlen) {
  uint8_t *b = (uint8_t *)malloc(28 + 4 + 4 + tlen + 4 + 4);
  if (!b) return;
  size_t n = message_header(b, 0x3F3, s_counter++);
  put_u32le(b + n, s_slot); /* recipient = slot (CivSwitchLDN net_chat_public) */
  n += 4;
  put_u32le(b + n, (uint32_t)tlen);
  n += 4;
  memcpy(b + n, text, tlen);
  n += tlen;
  put_u32le(b + n, 0xFFFFFFFE); /* chat_type -2 */
  n += 4;
  put_u32le(b + n, 0xFFFFFFFF); /* target -1 */
  n += 4;
  queue_msg(b, n);
  free(b);
}

/* --------------------------------------------------------------- helpers */

static void wait_ms(uint32_t ms, YieldFn yieldUi) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    if (yieldUi) yieldUi();
    delay(20);
  }
}

/* --------------------------------------------------------------- handshake */

/* CivSwitchLDN's lobby.py warmup_arp: let the host's LDN IP stack learn our
 * MAC before the first FireCONN. */
static void send_arp() {
  uint8_t f[42];
  memcpy(f, s_hostMac, 6);
  memcpy(f + 6, s_staMac, 6);
  f[12] = 0x08;
  f[13] = 0x06;
  f[14] = 0x00;
  f[15] = 0x01; /* htype */
  f[16] = 0x08;
  f[17] = 0x00; /* ptype */
  f[18] = 6;
  f[19] = 4; /* hlen/plen */
  f[20] = 0x00;
  f[21] = 0x01; /* request */
  memcpy(f + 22, s_staMac, 6); /* sha */
  memcpy(f + 28, s_localIp, 4); /* spa */
  memset(f + 32, 0, 6);        /* tha */
  memcpy(f + 38, s_hostIp, 4); /* tpa */
  for (int i = 0; i < 2; i++) {
    esp_wifi_internal_tx(WIFI_IF_STA, f, sizeof(f));
    delay(100);
  }
}

bool civ7Handshake(LogFn log, YieldFn yieldUi, String *err) {
  char line[128];
  snprintf(line, sizeof(line), "handshake: host %u.%u.%u.%u -> local %u.%u.%u.%u",
           s_hostIp[0], s_hostIp[1], s_hostIp[2], s_hostIp[3], s_localIp[0],
           s_localIp[1], s_localIp[2], s_localIp[3]);
  if (log) log(line);

  send_arp();

  /* FireCONN, retried until FireRPLY/NetWelcome */
  static const uint8_t CONN[8] = {0x4E, 0x4E, 0x4F, 0x43,
                                  0x65, 0x72, 0x69, 0x46};
  for (int attempt = 0; attempt < 6 && !s_welcome; attempt++) {
    uint8_t body[8 + 8 + 8 + 4 + 141];
    memset(body, 0, sizeof(body));
    /* u64 sequence 0 */
    memcpy(body + 8, CONN, 8);
    ip_reversed(s_hostIp, body + 16); /* prefix: reversed host ip + 4 zeros */
    /* 4 zero bytes at 24 */
    uint8_t *intro = body + 28;
    put_u32le(intro, 0);                    /* intro id */
    memcpy(intro + 4, "Civ7-PC", 7);        /* identity (64) */
    memcpy(intro + 4 + 64, "Civ7-PC", 7);   /* name (64) */
    put_u32le(intro + 4 + 128, 6);          /* field84 */
    intro[4 + 132] = 0;                     /* flag */
    put_u32le(intro + 4 + 133, 0);          /* extra len */
    transport_send(body, sizeof(body), true);
    if (log) {
      snprintf(line, sizeof(line), "FireCONN sent (attempt %d)", attempt + 1);
      log(line);
    }
    for (int i = 0; i < 30 && !s_welcome; i++) wait_ms(100, yieldUi);
  }
  if (!s_welcome) {
    if (err) *err = "no FireRPLY/NetWelcome from the console";
    return false;
  }
  return true;
}

bool civ7InjectChat(NextChatLineFn next, void *user, LogFn log,
                    YieldFn yieldUi, String *err) {
  if (!s_welcome) {
    if (err) *err = "chat injection before NetWelcome";
    return false;
  }

  char line[128];
  char text[1200];

  snprintf(line, sizeof(line), "lobby joined, slot %u", (unsigned)s_slot);
  if (log) log(line);

  wait_ms(300, yieldUi);
  send_net_register();
  wait_ms(300, yieldUi);
  send_net_app_register();
  wait_ms(400, yieldUi);
  send_net_snapshot();

  /* CivSwitchLDN pacing: chat i is due at t0 + 1000 + 200*i. */
  uint64_t t0 = millis();
  for (int i = 0;; i++) {
    int n = next(i, text, sizeof(text), user);
    if (n == 0) break;
    if (n < 0) {
      if (err) *err = "chat payload read error";
      return false;
    }
    while (millis() - t0 < (uint64_t)(1000 + 200 * i)) wait_ms(20, yieldUi);
    send_net_chat(text, (size_t)n);
    if (log) {
      snprintf(line, sizeof(line), "NetChat %d (%u bytes)", i + 1,
               (unsigned)n);
      log(line);
    }
    wait_ms(200, yieldUi);
  }
  if (log) log("all chat messages sent");
  wait_ms(300, yieldUi);
  return true;
}

/* ------------------------------------------------------------------ state */

uint32_t civ7Slot() { return s_slot; }
bool civ7Welcome() { return s_welcome; }

void civ7SetNet(const uint8_t localIp[4], const uint8_t hostIp[4],
                const uint8_t hostMac[6], const uint8_t staMac[6]) {
  memcpy(s_localIp, localIp, 4);
  memcpy(s_hostIp, hostIp, 4);
  memcpy(s_hostMac, hostMac, 6);
  memcpy(s_staMac, staMac, 6);
  s_nextSeq = 1;
  s_counter = 0;
  s_welcome = false;
  s_slot = 1;
  for (size_t i = 0; i < TX_RING; i++) {
    free(s_tx[i]);
    s_tx[i] = nullptr;
    s_tx_len[i] = 0;
    s_tx_seq[i] = 0;
  }
}

} // namespace ldn
