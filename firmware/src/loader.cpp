#include "loader.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>
#include <netdb.h>
#include <netinet/in.h>
#include <SD.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <lvgl.h>

#include "net.h"
#include "config.h"
#include "bundle.h"
#include "cfg.h"
#include "hw.h"

namespace loader {

namespace {

constexpr int kSendBuf = 2048;
constexpr int kIoTimeoutMs = 250; /* recv/snd slice; loop stays UI-responsive */

void fdSetIoTimeouts(int fd) {
  struct timeval tv;
  tv.tv_sec = kIoTimeoutMs / 1000;
  tv.tv_usec = (kIoTimeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool connectFd(int fd, uint32_t timeoutMs, String *err) {
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(cfg::consolePort);
  a.sin_addr.s_addr = inet_addr(net::targetIp().c_str());

  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  int r = connect(fd, (sockaddr *)&a, sizeof(a));
  if (r < 0 && errno != EINPROGRESS) {
    *err = String("connect errno ") + errno;
    return false;
  }
  if (r != 0) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    r = select(fd + 1, nullptr, &w, nullptr, &tv);
    if (r <= 0) {
      *err = "connect timeout";
      return false;
    }
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
    if (soerr) {
      *err = String("connect error ") + soerr;
      return false;
    }
  }
  fcntl(fd, F_SETFL, flags); /* back to blocking + SO_* timeouts */
  return true;
}

void yieldUi() { hw::lvLoop(); }

} // namespace

bool sendFile(const char *path, LogFn log, void *user, String *err,
              uint32_t timeoutMs) {
  hw::busForSd();
  File f = SD.open(path, FILE_READ);
  if (!f) {
    *err = String("cannot open ") + path;
    return false;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    f.close();
    *err = String("socket errno ") + errno;
    return false;
  }

  if (!connectFd(fd, CONNECT_TIMEOUT_MS, err)) {
    close(fd);
    f.close();
    return false;
  }
  fdSetIoTimeouts(fd);
  log("[connected - sending]", user);

  /* ---- send bytes ---- */
  uint8_t buf[kSendBuf];
  bool sendOk = true;
  uint32_t sendDeadline = millis() + 30000;
  while (sendOk && f.available()) {
    hw::busForSd();
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    int off = 0;
    while (off < n) {
      int s = send(fd, buf + off, n - off, 0);
      if (s <= 0) {
        if (s < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
          if (millis() > sendDeadline) {
            *err = "send timeout";
            sendOk = false;
          }
          yieldUi();
          break;
        }
        *err = String("send errno ") + errno;
        sendOk = false;
        break;
      }
      off += s;
    }
  }
  f.close();

  /* ---- half-close: this is how the loader sees end-of-payload ---- */
  if (sendOk) shutdown(fd, SHUT_WR);

  /* ---- pump the log stream ---- */
  char lineBuf[512];
  size_t lineLen = 0;
  auto emitLine = [&](const char *line) { log(line, user); };

  uint32_t deadline = millis() + timeoutMs;
  while (sendOk && millis() < deadline) {
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) break; /* console closed */
    if (n < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        yieldUi();
        continue;
      }
      *err = String("recv errno ") + errno;
      sendOk = false;
      break;
    }
    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n') {
        lineBuf[lineLen] = 0;
        emitLine(lineBuf);
        lineLen = 0;
      } else if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      }
    }
    yieldUi();
  }
  if (sendOk && lineLen > 0) {
    lineBuf[lineLen] = 0;
    emitLine(lineBuf);
  }

  close(fd);
  if (sendOk && millis() >= deadline) *err = "log read timeout";
  return sendOk;
}

bool ping(uint32_t timeoutMs) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  String err;
  bool ok = connectFd(fd, timeoutMs, &err);
  close(fd);
  if (!ok) Serial.printf("[loader] ping: %s\r\n", err.c_str());
  return ok;
}

/* ---- installBundle ------------------------------------------------------ */

namespace {

bool sendAllFd(int fd, const uint8_t *buf, size_t len, uint32_t deadline,
               String *err) {
  size_t off = 0;
  while (off < len) {
    int s = send(fd, buf + off, len - off, 0);
    if (s <= 0) {
      if (s < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        if (millis() > deadline) {
          *err = "send timeout";
          return false;
        }
        yieldUi();
        continue;
      }
      *err = String("send errno ") + errno;
      return false;
    }
    off += s;
  }
  return true;
}

bool sendAllFdStr(int fd, const char *str, uint32_t deadline, String *err) {
  return sendAllFd(fd, (const uint8_t *)str, strlen(str), deadline, err);
}

/* recv until the console closes or deadline; streams log lines, and keeps the
 * last `collectCap` bytes of the response for success/error inspection. */
bool readResponseFd(int fd, LogFn log, void *user, uint32_t timeoutMs,
                    String *collected, String *err) {
  char lineBuf[512];
  size_t lineLen = 0;
  uint8_t buf[1024];
  uint32_t deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        yieldUi();
        continue;
      }
      *err = String("recv errno ") + errno;
      return false;
    }
    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n') {
        lineBuf[lineLen] = 0;
        log(lineBuf, user);
        if (collected->length() > 4096) collected->remove(0, 2048);
        *collected += lineBuf;
        *collected += "\n";
        lineLen = 0;
      } else if (lineLen < sizeof(lineBuf) - 1) {
        lineBuf[lineLen++] = c;
      }
    }
    yieldUi();
  }
  if (lineLen > 0) {
    lineBuf[lineLen] = 0;
    log(lineBuf, user);
    if (collected->length() > 4096) collected->remove(0, 2048);
    *collected += lineBuf;
    *collected += "\n";
  }
  return true;
}

/* One loader connection: send a message, half-close, read the response. */
bool exchange(const char *msg, size_t len, bool readPayloadOk,
              LogFn log, void *user, uint32_t timeoutMs,
              String *response, String *err) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *err = String("socket errno ") + errno;
    return false;
  }
  if (!connectFd(fd, CONNECT_TIMEOUT_MS, err)) {
    close(fd);
    return false;
  }
  fdSetIoTimeouts(fd);

  uint32_t deadline = millis() + timeoutMs;
  bool ok = sendAllFd(fd, (const uint8_t *)msg, len, deadline, err);
  if (ok) shutdown(fd, SHUT_WR);
  if (ok) ok = readResponseFd(fd, log, user, timeoutMs, response, err);
  close(fd);
  return ok;
}
} // namespace

bool installBundle(LogFn log, ProgressFn progress, void *user, String *err,
                   uint32_t timeoutMs) {
  File tpl = SD.open(bundle::TPL_PATH, FILE_READ);
  if (!tpl) {
    *err = String("cannot open template b64 (") + bundle::TPL_PATH + ")";
    return false;
  }
  size_t tplLen = tpl.size();
  int total = (int)((tplLen + INSTALL_CHUNK - 1) / INSTALL_CHUNK);
  log((String("template: ") + tplLen + " b64 chars, " + total + " chunks").c_str(), user);

  /* ---- 1. reset: clear any previous template on the console ---- */
  String resp;
  hw::busForSd();
  progress(0, total, "reset", user);
  if (!exchange("log('STARTING INSTALLER');window.__tpl=null;'reset'",
                strlen("log('STARTING INSTALLER');window.__tpl=null;'reset'"),
                false, log, user, timeoutMs, &resp, err)) {
    tpl.close();
    return false;
  }
  log("loader reset ok", user);

  /* ---- 2. chunked template upload ---- */
  static const char *kPushPrefix = "(window.__tpl=window.__tpl||[]).push('";
  static const char *kPushSuffixFmt = "');'chunk %d/%d'";
  char suffix[32];

  bool ok = true;
  for (int i = 0; i < total && ok; i++) {
    size_t off = (size_t)i * INSTALL_CHUNK;
    size_t thisLen = (tplLen - off < INSTALL_CHUNK) ? (tplLen - off) : INSTALL_CHUNK;
    snprintf(suffix, sizeof(suffix), kPushSuffixFmt, i + 1, total);

    /* Reconnect per chunk: one message per connection. */
    hw::busForSd();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      *err = String("socket errno ") + errno;
      ok = false;
      break;
    }
    if (!connectFd(fd, CONNECT_TIMEOUT_MS, err)) {
      close(fd);
      ok = false;
      break;
    }
    fdSetIoTimeouts(fd);

    uint32_t deadline = millis() + timeoutMs;
    ok = sendAllFdStr(fd, kPushPrefix, deadline, err) &&
         (tpl.seek(off), true);
    while (ok && thisLen > 0) {
      uint8_t buf[2048];
      size_t want = thisLen < sizeof(buf) ? thisLen : sizeof(buf);
      int got = tpl.read(buf, (int)want);
      if (got <= 0) {
        *err = "template read failed";
        ok = false;
        break;
      }
      thisLen -= got;
      ok = sendAllFd(fd, buf, (size_t)got, deadline, err);
    }
    if (ok) ok = sendAllFdStr(fd, suffix, deadline, err);
    if (ok) shutdown(fd, SHUT_WR);
    if (ok) {
      resp = "";
      ok = readResponseFd(fd, log, user, timeoutMs, &resp, err);
      if (ok && resp.indexOf("Executed") < 0) {
        *err = String("chunk ") + (i + 1) + "/" + total +
               " not confirmed: " + resp.substring(0, 120);
        ok = false;
      }
    }
    close(fd);
    if (ok) progress(i + 1, total, "template", user);
  }
  tpl.close();
  if (!ok) return false;

  /* ---- 3. apply payload ---- */
  progress(total, total, "apply", user);
  hw::busForSd();
  File applyF = SD.open(bundle::APPLY_PATH, FILE_READ);
  if (!applyF) {
    *err = String("cannot open apply payload (") + bundle::APPLY_PATH + ")";
    return false;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    applyF.close();
    *err = String("socket errno ") + errno;
    return false;
  }
  if (!connectFd(fd, CONNECT_TIMEOUT_MS, err)) {
    close(fd);
    applyF.close();
    return false;
  }
  fdSetIoTimeouts(fd);

  uint32_t deadline = millis() + timeoutMs;
  bool done = true;
  uint8_t buf[2048];
  while (done && applyF.available()) {
    hw::busForSd();
    int n = applyF.read(buf, sizeof(buf));
    if (n <= 0) break;
    done = sendAllFd(fd, buf, (size_t)n, deadline, err);
  }
  if (done) shutdown(fd, SHUT_WR);
  resp = "";
  if (done) done = readResponseFd(fd, log, user, timeoutMs, &resp, err);
  applyF.close();
  close(fd);

  if (!done) return false;
  if (resp.indexOf("INSTALL DONE") < 0) {
    *err = "installer did not report INSTALL DONE: " + resp.substring(0, 160);
    return false;
  }
  log("INSTALL DONE confirmed by console", user);
  return true;
}

} // namespace loader
