/**
 * RGB LED status modes.
 * CYD LED pins: R=4, G=16, B=17. The LED is commonly wired active-low;
 * flip LED_ACTIVE_LOW if your unit is inverted.
 */
#pragma once

#include <cstdint>

namespace led {

enum Mode : uint8_t {
  OFF = 0,   ///< all dark
  IDLE,      ///< AP up, no station seen (slow green)
  SEEN,      ///< station on the AP (solid green)
  BUSY,      ///< SEND/INSTALL running (blinking blue)
  OK,        ///< last flow succeeded (solid green)
  FAIL,      ///< last flow failed (solid red)
};

void init();
void set(Mode m);
void tick(); ///< call from loop(); drives blinking modes

} // namespace led
