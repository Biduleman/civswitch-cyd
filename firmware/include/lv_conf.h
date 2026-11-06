/**
 * LVGL 8 configuration for the CivSwitch CYD host.
 * Deliberately minimal: LVGL's lv_conf_internal.h supplies defaults for
 * anything not defined here.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* 16-bit RGB565 to match the ILI9341 frame writes (byte swap handled by
 * TFT_eSPI's setSwapBytes(true), not by LVGL). */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Static LVGL pool; the appliance UI is small. (Kept modest: the ESP32's
 * DRAM segment also carries the WiFi stack's static reservations.) */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (40U * 1024U)

/* Tick from Arduino millis(). */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* Quiet. */
#define LV_USE_LOG 0

/* The default font used by labels/buttons. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#endif /* LV_CONF_H */
