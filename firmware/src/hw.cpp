#include "hw.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <lvgl.h>
#include <TFT_eSPI.h>

#include "config.h"

namespace hw {

namespace {

TFT_eSPI tft;

/* Shared VSPI: SD card (SCK 18 / MISO 19 / MOSI 23 / CS 5) and the XPT2046
 * touch controller (CLK 25 / MISO 39 / MOSI 32 / CS 33) take turns; the pin
 * matrix is re-mapped by busForSd()/busForTouch(). */
SPIClass busSpi(VSPI);
bool busOnTouch = false;
bool sdOk = false;

/* LVGL display driver: one partial buffer of 20 lines (12.8 KB).
 * Heap-allocated at init (NOT static): static DRAM must be reserved for the
 * WiFi stack, and the linker overflows if this array lives in .bss. */
constexpr int kBufLines = 20;
constexpr size_t kBufBytes = sizeof(lv_color_t) * kBufLines * 320;
lv_disp_draw_buf_t drawBuf;
lv_color_t *drawLine = nullptr;
lv_disp_drv_t dispDrv;
lv_indev_drv_t indevDrv;

void busForTouch() {
  if (!busOnTouch) {
    busSpi.end();
    busSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    busOnTouch = true;
  }
}

/* Canonical hardware-SPI XPT2046 read: 8-bit command + 16 clocks
 * (1 busy + 12 data MSB-first + 3 padding) -> drop 3 bits. */
uint16_t xptRead(uint8_t cmd) {
  busForTouch();
  busSpi.beginTransaction(SPISettings(2500000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS, LOW);
  busSpi.transfer(cmd);
  uint16_t v = busSpi.transfer16(0x00);
  digitalWrite(TOUCH_CS, HIGH);
  busSpi.endTransaction();
  return (v >> 3) & 0x0FFF;
}

bool touchPressed() {
  busForTouch();
  /* PENIRQ first; fall back to a Z1 pressure read for units where the IRQ
   * line is not usable. */
  if (digitalRead(TOUCH_IRQ) == LOW) return true;
  return xptRead(0xB0) > 30;
}

int16_t touchMap(int32_t raw, int32_t lo, int32_t hi, int32_t span) {
  if (hi == lo) return 0;
  int32_t v = (raw - lo) * span / (hi - lo);
  if (v < 0) v = 0;
  if (v > span - 1) v = span - 1;
  return (int16_t)v;
}

/* This panel's axes are swapped (measured): 0xD0 -> vertical, 0x90 -> horizontal. */
void readAxes(uint16_t *vertical, uint16_t *horizontal) {
  *vertical = xptRead(0xD0);
  *horizontal = xptRead(0x90);
}

/* ---- LVGL callbacks ---------------------------------------------------- */

void flushCb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *colorP) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&colorP->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

void touchCb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t x = 0, y = 0;
  if (touchState(&x, &y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

} // namespace

void busForSd() {
  if (busOnTouch) {
    busSpi.end();
    busSpi.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
    busOnTouch = false;
  }
}

bool init() {
  /* Backlight + panel. */
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);
  tft.setSwapBytes(true);
  Serial.println("[hw] display: ILI9341_2 ready");

  /* LVGL. */
  drawLine = (lv_color_t *)malloc(kBufBytes);
  if (!drawLine) {
    Serial.println("[hw] lvgl draw buffer alloc FAILED");
    return false;
  }
  lv_init();
  lv_disp_draw_buf_init(&drawBuf, drawLine, nullptr, kBufLines * 320);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = 320;
  dispDrv.ver_res = 240;
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchCb;
  lv_indev_drv_register(&indevDrv);

  /* Touch CS / IRQ are plain GPIO; the bus pins are mapped on demand. */
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  pinMode(TOUCH_IRQ, INPUT);
  Serial.println("[hw] lvgl + touch: ready");

  /* SD card first user of the shared bus. */
  busForSd();
  sdOk = SD.begin(SD_CS, busSpi, SD_FREQ);
  if (sdOk) {
    Serial.printf("[hw] sd: mounted, %llu MB\r\n",
                  (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
  } else {
    Serial.println("[hw] sd: MOUNT FAILED");
  }
  return sdOk;
}

bool sdReady() { return sdOk; }

uint64_t sdSizeBytes() {
  busForSd();
  return sdOk ? SD.cardSize() : 0;
}

void lvLoop() {
  lv_timer_handler();
  delay(5);
}

bool touchState(int16_t *x, int16_t *y) {
  if (!touchPressed()) return false;
  uint16_t v = 0, h = 0;
  readAxes(&v, &h);
  *x = touchMap(h, TOUCH_H_MIN, TOUCH_H_MAX, 320);
  *y = touchMap(v, TOUCH_V_MIN, TOUCH_V_MAX, 240);
  return true;
}

} // namespace hw
