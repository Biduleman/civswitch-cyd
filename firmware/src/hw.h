/**
 * Hardware bring-up for the CYD: display (ILI9341_2 via TFT_eSPI on HSPI),
 * LVGL 8 glue, XPT2046 touch and SD card — both on the shared VSPI bus.
 *
 * Bus sharing: the ESP32 has two usable SPI peripherals; the display owns
 * HSPI, so SD and touch share VSPI with a pin re-map around each user
 * (hw::busForSd() / busForTouch()). Touch uses the canonical hardware-SPI
 * XPT2046 reads proven in the TouchTest project.
 */
#pragma once

#include <cstdint>

namespace hw {

/** Returns true when everything mounted; details go to Serial either way. */
bool init();

/** Re-map the shared VSPI bus onto the SD card pins. Call before SD use. */
void busForSd();

bool sdReady();
uint64_t sdSizeBytes();

/** Pump LVGL; call from loop(). */
void lvLoop();

/** True while the panel is pressed; coords are mapped screen pixels. */
bool touchState(int16_t *x, int16_t *y);

} // namespace hw
