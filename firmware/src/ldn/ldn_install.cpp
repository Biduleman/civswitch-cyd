/*
 * Installer streaming over UDP :9027, ported from SeanHeuc's esp32-ldn-civ7
 * main/installer.c (© Sean Heuc) - the UDP path only; the CYD reads the
 * blobs from the SD bundle instead of flash partitions and the wire format
 * is unchanged.
 * SPDX-License-Identifier: GPL-3.0 (CivSwitchLDN lineage - see ldn.h).
 * Ticket 11.
 */
#include "ldn_install.h"

#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mbedtls/base64.h>

#include "../bundle.h"
#include "../hw.h"

namespace ldn {

namespace {

constexpr uint16_t INSTALL_PORT = 9027;
constexpr size_t UDP_CHUNK = 1100;
constexpr int REPAIR_ROUNDS = 60;

volatile bool s_done;      /* console sent 'D' */
QueueHandle_t s_reqQ;      /* NAKs: bit16 = js, low16 = chunk index */
File s_tplFile, s_jsFile;
uint32_t s_tplLen, s_jsLen;
int s_needT, s_needJ;

LogFn s_log = nullptr;
YieldFn s_yield = nullptr;
ProgressFn s_progress = nullptr;

void waitMs(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    if (s_yield) s_yield();
    delay(20);
  }
}

/* wifi rx callback context: parse only, never log via LogFn */
void installerUdpRx(const uint8_t *data, size_t len) {
  if (len == 1 && data[0] == 'D') {
    s_done = true;
    return;
  }
  if (len < 2 || (data[0] != 't' && data[0] != 'j')) return;
  char tmp[12];
  size_t n = len - 1;
  if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
  memcpy(tmp, data + 1, n);
  tmp[n] = 0;
  int idx = atoi(tmp);
  if (idx < 0 || !s_reqQ) return;
  uint32_t v = ((data[0] == 'j') ? (1u << 16) : 0u) | ((uint32_t)idx & 0xffff);
  xQueueSend(s_reqQ, &v, 0);
}

void sendProgress(int done, int total) {
  if (s_progress) s_progress(done, total);
}

bool sendChunk(bool js, int i) {
  File &f = js ? s_jsFile : s_tplFile;
  uint32_t total = js ? s_jsLen : s_tplLen;
  int need = js ? s_needJ : s_needT;
  if (i < 0 || i >= need) return false;

  static uint8_t buf[16 + UDP_CHUNK];
  int pfx = snprintf((char *)buf, sizeof(buf), js ? "J%d:" : "T%d:", i);

  if (js) {
    /* The console base64-decodes the joined J chunks (SeanHeuc's blobs
     * carry base64 TEXT of apply.js, built by his make_install_blobs.py;
     * the SD bundle keeps apply.js raw for the TCP loader path). Encode on
     * the fly: 825 source bytes -> exactly 1100 b64 chars per full chunk. */
    constexpr size_t QUANTUM = 825; /* 825 * 4/3 = 1100 */
    static uint8_t src[QUANTUM];
    uint32_t off = (uint32_t)i * QUANTUM;
    if (off >= s_jsLen) return false;
    size_t slen = s_jsLen - off;
    if (slen > QUANTUM) slen = QUANTUM;
    hw::busForSd();
    if (!f.seek(off)) return false;
    if (f.read(src, slen) != (int)slen) return false;
    size_t olen = 0;
    if (mbedtls_base64_encode(buf + pfx, sizeof(buf) - pfx, &olen, src,
                              slen) != 0)
      return false;
    int r = -1;
    for (int k = 0; k < 20 && r != 0; k++) {
      r = udpSend(INSTALL_PORT, buf, pfx + olen);
      if (r != 0) delay(1);
    }
    return r == 0;
  }

  /* T chunks: tpl_b64.txt is already the base64 text the console joins. */
  uint32_t off = (uint32_t)i * UDP_CHUNK;
  size_t len = total - off;
  if (len > UDP_CHUNK) len = UDP_CHUNK;
  hw::busForSd();
  if (!f.seek(off)) return false;
  int got = f.read(buf + pfx, len);
  if (got != (int)len) return false;

  int r = -1;
  for (int k = 0; k < 20 && r != 0; k++) {
    r = udpSend(INSTALL_PORT, buf, pfx + len);
    if (r != 0) delay(1);
  }
  return r == 0;
}

} // namespace

bool installerRun(LogFn log, YieldFn yieldUi, ProgressFn progress, String *err) {
  s_log = log;
  s_yield = yieldUi;
  s_progress = progress;
  if (err) *err = "";

  hw::busForSd();
  s_tplFile = SD.open(bundle::TPL_PATH, FILE_READ);
  s_jsFile = SD.open(bundle::APPLY_PATH, FILE_READ);
  s_tplLen = s_tplFile ? s_tplFile.size() : 0;
  s_jsLen = s_jsFile ? s_jsFile.size() : 0;
  if (!s_tplLen || !s_jsLen) {
    if (err) *err = "install blobs missing on SD (tpl_b64.txt / apply.js)";
    return false;
  }
  s_needT = (s_tplLen + UDP_CHUNK - 1) / UDP_CHUNK;
  /* the J chunks carry base64 text of apply.js (encoded on the fly in
   * 825-byte quanta -> 1100 chars per full chunk) */
  uint32_t jsB64Len = 4u * ((s_jsLen + 2) / 3);
  s_needJ = (jsB64Len + UDP_CHUNK - 1) / UDP_CHUNK;
  if (s_needT > 0xffff || s_needJ > 0xffff) {
    if (err) *err = "install blob too large for the chunk protocol";
    return false;
  }

  char line[160];
  snprintf(line, sizeof(line), "installer: tpl %u b64 chars (%d chunks), "
                               "apply %u bytes -> %u b64 chars (%d chunks)",
           (unsigned)s_tplLen, s_needT, (unsigned)s_jsLen,
           (unsigned)jsB64Len, s_needJ);
  if (log) log(line);

  s_done = false;
  s_reqQ = xQueueCreate(512, sizeof(uint32_t));
  setInstallerRx(installerUdpRx);

  /* let the chat payload's UDP receiver come up (upstream: 800 ms) */
  waitMs(800);

  const uint8_t *ip = localIp();
  char hdr[64];
  int hl = snprintf(hdr, sizeof(hdr), "A%d:%d:%u.%u.%u.%u", s_needT, s_needJ,
                    ip[0], ip[1], ip[2], ip[3]);
  for (int k = 0; k < 3; k++) {
    udpSend(INSTALL_PORT, (const uint8_t *)hdr, hl);
    waitMs(100);
  }
  if (log) {
    snprintf(line, sizeof(line), "header %s", hdr);
    log(line);
  }

  /* one fast pass: the receiver NAKs whatever the radio dropped */
  int total = s_needT + s_needJ;
  sendProgress(0, total);
  for (int i = 0; i < s_needJ && !s_done; i++) {
    sendChunk(true, i);
    if ((i % 25) == 0 && s_yield) s_yield();
    sendProgress(i + 1, total);
    delay(1);
  }
  for (int i = 0; i < s_needT && !s_done; i++) {
    sendChunk(false, i);
    if ((i % 25) == 0 && s_yield) s_yield();
    sendProgress(s_needJ + i + 1, total);
    delay(1);
  }
  if (log) log("fast pass done, waiting for NAKs");

  /* repair window: poll G, drain the receiver's missing-chunk requests */
  int rounds = 0;
  while (!s_done && rounds < REPAIR_ROUNDS) {
    udpSend(INSTALL_PORT, (const uint8_t *)"G", 1);
    waitMs(150);
    uint32_t v;
    int miss = 0;
    while (xQueueReceive(s_reqQ, &v, 0) == pdTRUE) {
      sendChunk((v >> 16) != 0, (int)(v & 0xffff));
      delay(2);
      miss++;
    }
    if (miss) {
      if (log) {
        snprintf(line, sizeof(line), "repair round %d: %d chunk(s) resent",
                 rounds + 1, miss);
        log(line);
      }
      sendProgress(total - miss < 0 ? 0 : total - miss, total);
    }
    rounds++;
  }

  s_tplFile.close();
  s_jsFile.close();
  setInstallerRx(nullptr);
  vQueueDelete(s_reqQ);
  s_reqQ = nullptr;

  if (!s_done) {
    if (err) *err = "installer not acknowledged (no 'D' from the console)";
    sendProgress(0, 0);
    return false;
  }
  sendProgress(total, total);
  if (log) {
    snprintf(line, sizeof(line), "installer acknowledged after %d repair round(s)",
             rounds);
    log(line);
  }
  return true;
}

} // namespace ldn
