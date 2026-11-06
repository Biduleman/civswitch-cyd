#include "led.h"

#include <Arduino.h>

#include "config.h"

namespace led {

namespace {

constexpr uint8_t kPins[3] = {LED_R, LED_G, LED_B};
Mode mode = OFF;
bool blinkOn = false;
uint32_t lastBlink = 0;

void writeRgb(bool r, bool g, bool b) {
#if LED_ACTIVE_LOW
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
#else
  digitalWrite(LED_R, r ? HIGH : LOW);
  digitalWrite(LED_G, g ? HIGH : LOW);
  digitalWrite(LED_B, b ? HIGH : LOW);
#endif
}

void apply() {
  switch (mode) {
  case OFF:  writeRgb(false, false, false); break;
  case IDLE: writeRgb(false, blinkOn, false); break;  /* slow green pulse-ish */
  case SEEN: writeRgb(false, true, false); break;
  case BUSY: writeRgb(false, false, blinkOn); break;  /* blinking blue */
  case OK:   writeRgb(false, true, false); break;
  case FAIL: writeRgb(true, false, false); break;
  }
}

} // namespace

void init() {
  for (uint8_t p : kPins) pinMode(p, OUTPUT);
  mode = OFF;
  apply();
}

void set(Mode m) {
  if (m == mode) return;
  mode = m;
  lastBlink = millis();
  blinkOn = true;
  apply();
}

void tick() {
  switch (mode) {
  case IDLE:
  case BUSY:
    if (millis() - lastBlink >= (mode == BUSY ? 150 : 700)) {
      lastBlink = millis();
      blinkOn = !blinkOn;
      apply();
    }
    break;
  default:
    break;
  }
}

} // namespace led
