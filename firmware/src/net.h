/**
 * Network layer: the offline access point the Switch joins.
 * Offline access point for the Switch.
 *
 * Load-bearing detail: DHCP MUST hand out the gateway option
 * (the AP's own IP). The CivSwitch loader probes its own IP with a UDP
 * connect() to 8.8.8.8, which fails without a default route. softAPConfig
 * with gateway == AP IP does this; verify on hardware at acceptance.
 */
#pragma once

#include <Arduino.h>

namespace net {

/** Brings the AP up; returns false if softAP failed. */
bool apStart();

bool apUp();
String apIp();

/** "" until a station has been assigned an address by DHCP. */
String stationIp();
bool stationSeen();

/**
 * The console IP to target: the DHCP-assigned station IP when the AP has
 * handed one out, otherwise the configured fallback (a console set to a
 * manual IP produces no DHCP event, so the fallback covers it).
 */
String targetIp();

} // namespace net
