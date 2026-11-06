/**
 * CivSwitch CYD host - board constants.
 * Board pins + constants for the CYD (ESP32-2432S028R).
 */
#pragma once

#define FW_VERSION "1.0.0"

/* ---- Display (ILI9341, landscape via setRotation(1): 320x240) ---- */
#define TFT_ROTATION 1

/* ---- Bit-banged XPT2046 touch (own pins, no SPI bus sharing) ----
 * The CYD wires the touch controller to pins that are not on the display's
 * SPI bus, so we bit-bang it: 12-bit conversions are tiny and slow is fine. */
#define TOUCH_CS 33
#define TOUCH_CLK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39   /* input-only pin: fine for MISO */
#define TOUCH_IRQ 36    /* PENIRQ, active low */

/* Touch axis calibration — measured on this unit (landscape):
 * the XPT2046 "X" command (0xD0) reports the VERTICAL position and the
 * "Y" command (0x90) the HORIZONTAL one. Raw span is ~1000..3400;
 * widened slightly so the screen edges stay reachable. */
#define TOUCH_V_MIN 900   /* raw from 0xD0 at screen top */
#define TOUCH_V_MAX 3500  /* raw from 0xD0 at screen bottom */
#define TOUCH_H_MIN 900   /* raw from 0x90 at screen left */
#define TOUCH_H_MAX 3500  /* raw from 0x90 at screen right */

/* ---- RGB LED status ---- */
#define LED_R 4
#define LED_G 16
#define LED_B 17
#define LED_ACTIVE_LOW 1 /* flip if your unit is common-cathode */

/* ---- Access point (auto-starts on boot, no portal; hardcoded defaults, /config.txt overrides) ---- */
#define AP_SSID "CivSwitch"
#define AP_PASS "civswitch7273"
#define AP_CHANNEL 6
#define AP_MAX_STATIONS 4
#define WIFI_COUNTRY "US"
#define AP_GATEWAY "192.168.4.1"
#define AP_NETMASK "255.255.255.0"

/* ---- Console (Switch) target ---- */
#define CONSOLE_IP "192.168.4.200"
#define CONSOLE_PORT 9026
#define CONNECT_TIMEOUT_MS 8000
#define SEND_TIMEOUT_MS 120000
#define INSTALL_TIMEOUT_MS 300000
#define INSTALL_CHUNK 400000 /* base64 chars per loader connection */
#define LOADER_MAX_PAYLOAD (500U * 1024U)

/* ---- SD card (VSPI: SCK 18 / MISO 19 / MOSI 23 / CS 5) ---- */
#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define SD_FREQ 20000000 /* CYD cards can be flaky at 40 MHz */
