#include "cfg.h"

#include <SD.h>

#include "config.h"
#include "hw.h"

namespace cfg {

String ssid = AP_SSID;
String pass = AP_PASS;
String country = WIFI_COUNTRY;
String consoleIp = CONSOLE_IP;
int channel = AP_CHANNEL;
int consolePort = CONSOLE_PORT;

namespace {

void applyLine(const String &line) {
  int eq = line.indexOf('=');
  if (eq <= 0) return;
  String k = line.substring(0, eq);
  String v = line.substring(eq + 1);
  k.trim();
  v.trim();
  if (k.length() == 0 || v.length() == 0) return;
  if (k == "ssid") ssid = v;
  else if (k == "pass") {
    if (v.length() >= 8) pass = v; /* WPA2 needs >= 8 chars */
  } else if (k == "channel") {
    int c = v.toInt();
    if (c >= 1 && c <= 13) channel = c;
  } else if (k == "country") {
    if (v.length() == 2) country = v;
  } else if (k == "console_ip") {
    consoleIp = v;
  } else if (k == "console_port") {
    int p = v.toInt();
    if (p > 0 && p < 65536) consolePort = p;
  }
}

} // namespace

void load() {
  hw::busForSd();
  File f = SD.open("/config.txt", FILE_READ);
  if (!f) return; /* defaults apply */
  Serial.println("[cfg] loading /config.txt overrides");
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.replace("\r", "");
    int hash = line.indexOf('#');
    if (hash >= 0) line = line.substring(0, hash);
    applyLine(line);
  }
  f.close();
  Serial.printf("[cfg] ssid=%s channel=%d console=%s:%d\r\n", ssid.c_str(),
                channel, consoleIp.c_str(), consolePort);
}

} // namespace cfg
