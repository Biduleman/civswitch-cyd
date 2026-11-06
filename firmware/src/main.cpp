#include <Arduino.h>
#include <SD.h>
#include <lvgl.h>
#include <vector>

#include "bundle.h"
#include "cfg.h"
#include "config.h"
#include "hw.h"
#include "led.h"
#include "loader.h"
#include "net.h"

// CivSwitch CYD host - INSTALL flow.

static lv_obj_t *scrMenu;
static lv_obj_t *scrLog;
static lv_obj_t *apLabel;
static lv_obj_t *stationLabel;
static lv_obj_t *installBtn;
static lv_obj_t *flowStatusLabel; /* lives on the log screen */
static lv_obj_t *bar;
static lv_obj_t *barLabel;
static lv_obj_t *logLabel;
static std::vector<lv_obj_t *> payloadButtons;
static bool flowBusy = false;
static String logText;
static String manifestSource = "unknown";
static String manifestSha;
static File runFile;

/* Run-log capture: every loader line is appended to
 * /runs/run-<millis>.log for the duration of a flow. */
static void runLogOpen() {
  if (!hw::sdReady()) return;
  hw::busForSd();
  SD.mkdir("/runs");
  String name = String("/runs/run-") + String(millis()) + ".log";
  runFile = SD.open(name, FILE_WRITE);
  if (runFile) Serial.printf("[runs] capturing to %s\r\n", name.c_str());
}

static void runLogClose() {
  if (runFile) runFile.close();
}

/* ---- manifest provenance (written by build_sd.py) ---- */

static void readManifest() {
  hw::busForSd();
  File f = SD.open(bundle::MANIFEST_PATH, FILE_READ);
  if (!f) return;
  String j;
  while (f.available()) j += (char)f.read();
  f.close();
  if (j.length() > 2048) return;

  auto extract = [&j](const char *key) -> String {
    int k = j.indexOf(String("\"") + key + "\"");
    if (k < 0) return "";
    int colon = j.indexOf(':', k);
    int q1 = j.indexOf('"', colon);
    if (q1 < 0) return "";
    int q2 = j.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    return j.substring(q1 + 1, q2);
  };
  String src = extract("source_payload");
  String sha = extract("stage_sha256");
  if (src.length()) manifestSource = src;
  if (sha.length()) manifestSha = sha;
}

/* ---- on-screen log: last lines on screen; full log mirrors to serial ---- */

static void uiLogAppend(const char *line) {
  Serial.printf("[loader] %s\r\n", line);
  if (runFile) {
    hw::busForSd();
    runFile.println(line);
  }
  logText += line;
  logText += "\n";
  while (logText.length() > 1200) logText.remove(0, logText.indexOf('\n') + 1);
  lv_label_set_text(logLabel, logText.c_str());
  lv_obj_scroll_to_y(logLabel, LV_COORD_MIN, LV_ANIM_OFF);
}

static void logLineCb(const char *line, void *) { uiLogAppend(line); }

static void progressCb(int done, int total, const char *phase, void *) {
  lv_label_set_text_fmt(barLabel, "%s %d/%d", phase, done, total);
  lv_bar_set_value(bar, total ? done * 100 / total : 0, LV_ANIM_OFF);
}

static void setFlowEnabled(bool enabled) {
  for (lv_obj_t *b : payloadButtons) {
    if (enabled) lv_obj_clear_state(b, LV_STATE_DISABLED);
    else lv_obj_add_state(b, LV_STATE_DISABLED);
  }
  if (enabled) lv_obj_clear_state(installBtn, LV_STATE_DISABLED);
}

static void runSend(const char *path) {
  flowBusy = true;
  led::set(led::BUSY);
  setFlowEnabled(false);
  lv_label_set_text(flowStatusLabel, "busy: sending payload...");
  lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(barLabel, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(scrLog);
  runLogOpen();

  String err;
  bool ok = loader::sendFile(path, logLineCb, nullptr, &err, SEND_TIMEOUT_MS);
  if (!ok) uiLogAppend((String("SEND FAILED: ") + err).c_str());

  lv_label_set_text(flowStatusLabel, ok ? "send finished" : "send failed");
  lv_obj_set_style_text_color(flowStatusLabel,
                              ok ? lv_palette_main(LV_PALETTE_GREEN)
                                 : lv_palette_main(LV_PALETTE_RED),
                              0);
  led::set(ok ? led::OK : led::FAIL);
  runLogClose();
  setFlowEnabled(true);
  flowBusy = false;
}

static void runInstall() {
  flowBusy = true;
  led::set(led::BUSY);
  setFlowEnabled(false);
  runLogOpen();
  lv_label_set_text(flowStatusLabel, "busy: installing...");
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(barLabel, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load(scrLog);

  String err;
  bool ok = loader::installBundle(logLineCb, progressCb, nullptr, &err,
                                  INSTALL_TIMEOUT_MS);
  lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(barLabel, LV_OBJ_FLAG_HIDDEN);

  if (ok) {
    uiLogAppend("INSTALL DONE");
    lv_label_set_text(flowStatusLabel,
                      "install finished - reboot console, Load Game");
    lv_obj_set_style_text_color(flowStatusLabel,
                                lv_palette_main(LV_PALETTE_GREEN), 0);
  } else {
    uiLogAppend((String("INSTALL FAILED: ") + err).c_str());
    lv_label_set_text(flowStatusLabel, "install failed");
    lv_obj_set_style_text_color(flowStatusLabel,
                                lv_palette_main(LV_PALETTE_RED), 0);
  }
  led::set(ok ? led::OK : led::FAIL);
  runLogClose();
  setFlowEnabled(true);
  flowBusy = false;
}

static void sendHandler(lv_event_t *e) {
  if (flowBusy) return;
  const char *path = (const char *)lv_event_get_user_data(e);
  runSend(path);
}

static void installClick(lv_event_t *e) {
  (void)e;
  if (flowBusy) return;

  /* click-time loader check, then straight into the install. The installer
   * wipes all single-player saves and replaces the boot payload - logged
   * before it runs. */
  flowBusy = true;
  setFlowEnabled(false);
  lv_label_set_text(flowStatusLabel, "checking for loader...");
  lv_scr_load(scrLog);
  uiLogAppend("checking for loader...");
  bool live = loader::ping(3000);
  if (!live) {
    uiLogAppend((String("no loader detected at ") + net::targetIp() + ":" +
                 CONSOLE_PORT + " - bootstrap the console first (Pico or LDN)")
                    .c_str());
    lv_label_set_text(flowStatusLabel, "no loader");
    lv_obj_set_style_text_color(flowStatusLabel,
                                lv_palette_main(LV_PALETTE_RED), 0);
    flowBusy = false;
    setFlowEnabled(true);
    return;
  }
  uiLogAppend("loader detected");
  uiLogAppend("WARNING: this wipes all single-player saves and replaces the "
              "boot payload");
  String bootLine = String("boot payload: ") + manifestSource + "  sha256 " +
                    manifestSha.substring(0, 16);
  uiLogAppend(bootLine.c_str());
  runInstall();
}

static void backHandler(lv_event_t *e) {
  (void)e;
  if (!flowBusy) lv_scr_load(scrMenu);
}

static void buildUi(bool sd) {
  scrMenu = lv_obj_create(nullptr);
  scrLog = lv_obj_create(nullptr);

  /* ---- menu screen ---- */
  lv_obj_t *header = lv_label_create(scrMenu);
  lv_label_set_text_fmt(header, "CivSwitch CYD host %s", FW_VERSION);
  lv_obj_set_pos(header, 8, 6);

  apLabel = lv_label_create(scrMenu);
  lv_obj_set_pos(apLabel, 8, 28);

  stationLabel = lv_label_create(scrMenu);
  lv_obj_set_pos(stationLabel, 8, 50);

  lv_obj_t *sdLabel = lv_label_create(scrMenu);
  lv_label_set_text_fmt(sdLabel, "SD: %s (%llu MB)",
                        sd ? "mounted" : "MISSING",
                        (unsigned long long)(hw::sdSizeBytes() / (1024ULL * 1024ULL)));
  lv_obj_set_pos(sdLabel, 8, 72);
  lv_obj_set_style_text_color(sdLabel,
                              sd ? lv_palette_main(LV_PALETTE_GREEN)
                                 : lv_palette_main(LV_PALETTE_RED),
                              0);

  /* The Switch line doubles as the target display: DHCP-assigned IP when
   * seen, otherwise the fallback IP (config.txt console_ip) is used. */

  flowStatusLabel = lv_label_create(scrLog);
  lv_obj_set_pos(flowStatusLabel, 80, 11);
  lv_obj_set_style_text_color(flowStatusLabel, lv_palette_main(LV_PALETTE_GREY), 0);

  if (!sd) {
    lv_obj_t *err = lv_label_create(scrMenu);
    lv_label_set_text(err, "SD card missing - cannot scan payloads.\n"
                           "Run build_sd.py and copy the bundle.");
    lv_obj_set_pos(err, 8, 96);
    lv_obj_set_style_text_color(err, lv_palette_main(LV_PALETTE_RED), 0);
    return;
  }

  std::vector<bundle::PayloadInfo> payloads;
  if (!bundle::scanPayloads(payloads) || payloads.empty()) {
    lv_obj_t *err = lv_label_create(scrMenu);
    lv_label_set_text(err, "No payloads found on the SD card.\n"
                           "Run build_sd.py and copy the bundle.");
    lv_obj_set_pos(err, 8, 96);
    lv_obj_set_style_text_color(err, lv_palette_main(LV_PALETTE_ORANGE), 0);
    return;
  }

  int slots = payloads.size() < 8 ? payloads.size() : 8;
  for (int i = 0; i < slots; i++) {
    const bundle::PayloadInfo &p = payloads[i];
    lv_obj_t *btn = lv_btn_create(scrMenu);
    lv_obj_set_size(btn, 148, 26);
    lv_obj_set_pos(btn, 8 + (i % 2) * 158, 96 + (i / 2) * 32);
    char *path = strdup(p.path.c_str());
    lv_obj_add_event_cb(btn, sendHandler, LV_EVENT_CLICKED, path);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text_fmt(l, "%s%s", p.name.c_str(), p.ok ? "" : " (too big)");
    lv_obj_center(l);
    if (!p.ok) lv_obj_add_state(btn, LV_STATE_DISABLED);
    payloadButtons.push_back(btn);
  }
  if ((int)payloads.size() > 8) {
    lv_obj_t *more = lv_label_create(scrMenu);
    lv_label_set_text_fmt(more, "+%d more on SD", (int)payloads.size() - 8);
    lv_obj_set_pos(more, 166, 218);
  }

  installBtn = lv_btn_create(scrMenu);
  lv_obj_set_size(installBtn, 148, 22);
  lv_obj_set_pos(installBtn, 8, 214);
  lv_obj_add_event_cb(installBtn, installClick, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *installLabel = lv_label_create(installBtn);
  lv_label_set_text(installLabel, "INSTALL");
  lv_obj_center(installLabel);

  /* ---- log screen ---- */
  lv_obj_t *back = lv_btn_create(scrLog);
  lv_obj_set_size(back, 64, 28);
  lv_obj_set_pos(back, 8, 4);
  lv_obj_add_event_cb(back, backHandler, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *backLabel = lv_label_create(back);
  lv_label_set_text(backLabel, "back");
  lv_obj_center(backLabel);

  bar = lv_bar_create(scrLog);
  lv_obj_set_size(bar, 230, 14);
  lv_obj_set_pos(bar, 82, 11);
  lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

  barLabel = lv_label_create(scrLog);
  lv_obj_set_pos(barLabel, 82, 28);
  lv_obj_add_flag(barLabel, LV_OBJ_FLAG_HIDDEN);

  logLabel = lv_label_create(scrLog);
  lv_label_set_text(logLabel, "");
  lv_obj_set_width(logLabel, 304);
  lv_obj_set_pos(logLabel, 8, 36);
  lv_label_set_long_mode(logLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(logLabel, lv_palette_main(LV_PALETTE_LIME), 0);
}

static void refreshNetLabels() {
  lv_label_set_text_fmt(apLabel, "AP: %s  pw: %s  %s", cfg::ssid.c_str(),
                        cfg::pass.c_str(),
                        net::apUp() ? net::apIp().c_str() : "DOWN");
  if (net::stationSeen()) {
    lv_label_set_text_fmt(stationLabel, "Switch: %s (target)",
                          net::stationIp().c_str());
    lv_obj_set_style_text_color(stationLabel,
                                lv_palette_main(LV_PALETTE_GREEN), 0);
  } else {
    lv_label_set_text_fmt(stationLabel,
                          "Switch: not seen - target %s",
                          cfg::consoleIp.c_str());
    lv_obj_set_style_text_color(stationLabel, lv_palette_main(LV_PALETTE_GREY), 0);
  }
}

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.printf("CivSwitch CYD host %s\r\n", FW_VERSION);

  bool sd = hw::init();
  led::init();
  led::set(led::IDLE);
  if (sd) cfg::load();
  if (sd) readManifest();
  bool ap = net::apStart();
  buildUi(sd);
  lv_scr_load(scrMenu); /* the built screen must actually become the active one */
  refreshNetLabels();
  if (!ap) lv_obj_set_style_text_color(apLabel, lv_palette_main(LV_PALETTE_RED), 0);
  Serial.printf("[boot] armed; boot payload: %s\r\n", manifestSource.c_str());
}

void loop() {
  hw::lvLoop();
  led::tick();

  uint32_t now = millis();
  static uint32_t lastNetUi = 0;
  if (now - lastNetUi >= 1000) {
    lastNetUi = now;
    refreshNetLabels();
  }
}
