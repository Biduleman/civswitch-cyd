/**
 * Optional /config.txt overrides + run-log capture.
 *
 * /config.txt (FAT32 SD root), "key=value" lines, '#' starts a comment:
 *   ssid=CivSwitch
 *   pass=civswitch7273
 *   channel=6
 *   country=US
 *   console_ip=192.168.4.200
 *   console_port=9026
 * Absent file -> firmware defaults (config.h).
 */
#pragma once

#include <Arduino.h>

namespace cfg {

extern String ssid;
extern String pass;
extern String country;
extern String consoleIp;
extern int channel;
extern int consolePort;

void load(); // call after SD is up, before the AP starts

} // namespace cfg
