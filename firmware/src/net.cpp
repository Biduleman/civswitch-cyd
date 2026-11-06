#include "net.h"

#include <WiFi.h>
#include <esp_wifi.h>

#include "cfg.h"
#include "config.h"

namespace net {

namespace {

bool apUpFlag = false;
volatile bool stationSeenFlag = false;
String stationIpStr;
bool eventHooked = false;

void onEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
    /* esp_ip4_addr_t.addr is the four octets in network byte order. */
    uint32_t a = info.wifi_ap_staipassigned.ip.addr;
    IPAddress assigned((uint8_t)((a >> 0) & 0xFF), (uint8_t)((a >> 8) & 0xFF),
                       (uint8_t)((a >> 16) & 0xFF), (uint8_t)((a >> 24) & 0xFF));
    stationIpStr = assigned.toString();
    stationSeenFlag = true;
    Serial.printf("[net] station assigned %s\r\n", stationIpStr.c_str());
  }
}

} // namespace

bool apStart() {
  WiFi.mode(WIFI_AP);

  IPAddress gw, mask;
  gw.fromString(AP_GATEWAY);
  mask.fromString(AP_NETMASK);
  if (!WiFi.softAPConfig(gw, gw, mask)) {
    Serial.println("[net] softAPConfig FAILED");
    return false;
  }

  /* Regulatory domain before the radio starts beaconing. */
  esp_wifi_set_country_code(cfg::country.c_str(), true);

  apUpFlag = WiFi.softAP(cfg::ssid.c_str(), cfg::pass.c_str(), cfg::channel, 0,
                         AP_MAX_STATIONS);
  if (!apUpFlag) {
    Serial.println("[net] softAP FAILED");
    return false;
  }

  if (!eventHooked) {
    WiFi.onEvent(onEvent);
    eventHooked = true;
  }

  Serial.printf("[net] AP '%s' up, gateway %s, channel %d\r\n",
                cfg::ssid.c_str(), AP_GATEWAY, cfg::channel);
  Serial.printf("[net] AP IP: %s\r\n", WiFi.softAPIP().toString().c_str());
  return true;
}

bool apUp() { return apUpFlag; }

String apIp() { return WiFi.softAPIP().toString(); }

String stationIp() { return stationSeenFlag ? stationIpStr : String(""); }

String targetIp() {
  return stationSeenFlag ? stationIpStr : String(cfg::consoleIp);
}

bool stationSeen() { return stationSeenFlag; }

} // namespace net
