# CivSwitch CYD host — firmware

Firmware for the Cheap Yellow Display (ESP32-2432S028R) acting as the
offline Wi-Fi host for CivSwitch. See the repo root README for the big
picture.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (CLI or VS Code extension):

```sh
pio run                 # compile
pio run -t upload       # flash over the CH340 USB port
pio device monitor      # 115200 baud serial log
```

Board config lives in `platformio.ini` (ESP32 `esp32dev`, CYD display pins,
`huge_app` partition table).

## Layout

- `src/main.cpp` — boot + top-level loop + screens (menu / log / confirm)
- `src/config.h` — board pins + fixed constants (defaults for `cfg`)
- `src/cfg.h/.cpp` — optional `/config.txt` overrides
- `src/hw.h/.cpp` — ILI9341_2 + LVGL glue, XPT2046 touch, SD card
- `src/net.h/.cpp` — SoftAP with gateway-bearing DHCP
- `src/bundle.h/.cpp` — SD bundle scan (SEND payloads, install artifacts)
- `src/loader.h/.cpp` — loader protocol client: SEND + INSTALL
- `src/led.h/.cpp` — RGB status LED modes
- `include/lv_conf.h` — LVGL 8 configuration
