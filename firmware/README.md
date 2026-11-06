# CivSwitch CYD host — firmware

Firmware for the Cheap Yellow Display (ESP32-2432S028R) acting as the
offline CivSwitch network host. The SD-card bundle it consumes is built by
`../tools/build_sd.py`; repo-level docs live in the [root README](../README.md).

## What it does

- **AP mode (default)** — softAP `CivSwitch` (WPA2, channel 6, US) that the
  Switch joins; the CivSwitch loader runs on the console at TCP `:9026`.
- **SEND** — stream a raw `.js` payload from the SD card to the loader.
- **INSTALL** — gated, chunked savefile-installer flow (writes the
  boot-persistent trigger) from the bundle artifacts.
- **BOOTSTRAP (LDN)** — one tap, no PC, no Pico: joins the Civ VII wireless
  lobby's LDN network (scan → advertisement decrypt → key derivation from
  `/prod.keys` → association → LDN authentication → FireCONN → NetWelcome),
  injects the payload as lobby-chat messages, streams the installer over
  UDP :9027, then reboots back into AP mode. The console ends up with the
  persistent trigger; after a console reboot + **Load Game** it rejoins the
  AP and SEND/INSTALL work again.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (CLI or VS Code extension):

```sh
pio run                 # compile
pio run -t upload       # flash over the CH340 USB port (add --upload-port COMx if needed)
pio device monitor      # 115200 baud serial log
```

If esptool fails to connect: hold **BOOT**, tap **RESET**, release **BOOT**,
retry. Board config lives in `platformio.ini` (ESP32 `esp32dev` + CYD
display/touch defines).

## SD card setup

Build the bundle from the repo root (needs `python3`; `--ldn` additionally
needs `node` + terser):

```sh
npm install terser          # in tools/ (or in your CivSwitch clone's PicoSwitch/)
python tools/build_sd.py --civswitch ../CivSwitch --stage build/stage.hex --ldn
```

then copy the **contents** of `build/sd/` onto the root of a FAT32 SD card,
and add your own `/prod.keys` (dumped from your Switch 1) at the root. The
card must end up with:

```
/prod.keys                  your keys - never committed, never baked
/payloads/*.js              SEND payloads
/install/stage.hex          (+ .sha256 sidecar)
/install/tpl_b64.txt        save template, base64
/install/apply.js           installer payload
/install/chat_payload.txt   LDN chat transport (from --ldn)
/manifest.json
```

`/prod.keys` is read at **runtime**; the four entries used are
`aes_kek_generation_source`, `aes_key_generation_source`, `master_key_00`,
`master_key_12` (the same ones SeanHeuc's `tools/make_keys.py` requires).
No key material is ever committed, baked into a binary, or logged.

## Running a bootstrap

1. Console: `MULTIPLAYER → WIRELESS → CREATE GAME → HOST LOBBY` (a **fresh**
   lobby per attempt — the payload replaces the lobby UI).
2. CYD: tap `BOOTSTRAP (LDN)`. The pre-flight checks the SD files; the
   screen shows the phases (`keys → radio → scan → decrypt → associate →
   auth → ip → chat → installer`) and the installer chunk bar.
3. Success: `INSTALL DONE` on the console, the CYD reboots back into AP
   mode. On the console: exit the game, reboot, **Load Game** — the trigger
   fires, the console joins the AP and the loader is on `:9026`; SEND and
   INSTALL work as usual.

Full logs land in serial + `/runs/run-<millis>.log` on the SD card.

### Troubleshooting

| Symptom | Cause |
|---|---|
| `no LDN advertisement found` | lobby closed / not hosted yet — re-create it |
| `advertisement sha mismatch` | wrong `/prod.keys` for this console |
| `LDN authentication failed` | wrong keys or the lobby changed — fresh lobby |
| `all association attempts failed` | lobby closed / channel changed |
| `no FireRPLY/NetWelcome` | the console left the lobby — restart the game |
| `missing /install/chat_payload.txt` | bundle built without `--ldn` |
| `installer not acknowledged` | chunks lost and the repair window expired — retry with the CYD closer to the console |
| AP never comes back after a run | it reboots automatically; if it hangs, power-cycle |

## Layout

- `src/main.cpp` — boot + top-level loop + screens (menu / log / LDN
  bootstrap) and the BOOTSTRAP / SEND / INSTALL button flows
- `src/config.h` — board pins + fixed constants (defaults for `cfg`)
- `src/cfg.h/.cpp` — optional `/config.txt` overrides
- `src/hw.h/.cpp` — ILI9341 + LVGL glue, bit-banged XPT2046 touch, SD card
- `src/net.h/.cpp` — SoftAP with gateway-bearing DHCP
- `src/bundle.h/.cpp` — SD bundle scan (SEND payloads, install artifacts)
- `src/loader.h/.cpp` — loader protocol client: SEND + INSTALL + liveness ping
- `src/led.h/.cpp` — RGB status LED modes
- `src/ldn/` — LDN bootstrap module; file-by-file licensing in the table
  below
  - `ldn.cpp` — flow: AP teardown, scan/decrypt/derive, association,
    authentication, IP pickup, chat injection
  - `ldn_civ7.h/.cpp` — Civ VII lobby protocol (Fire transport, FireCONN,
    NetWelcome, NetChat)
  - `ldn_crypto.h/.cpp` — Kinnay key derivation + AES/HMAC (mbedtls)
  - `ldn_keys.h/.cpp` — `/prod.keys` parser (runtime, no build-time baking)
  - `ldn_hooks.h/.cpp` + `ldn_priv.h` — handshake-less association hooks
  - `ldn_install.h/.cpp` — installer streaming over UDP :9027
- `include/lv_conf.h` — LVGL 8 configuration

## License

This firmware is licensed **AGPL-3.0-only** — see the [`LICENSE`](../LICENSE)
file at the repo root. The LDN stack ports code and protocols from upstream
projects; those components keep their upstream licenses and attribution
(notices are preserved in each file's header), so the combined work
acknowledges code taken from GPL-3.0-only, GPL-3.0, AGPL-3.0 and MIT
projects. Redistribution of a **binary** requires honoring all of them:

| Files | License | Upstream |
|---|---|---|
| `src/ldn/ldn.cpp` | GPL-3.0 | protocol flow ported from [SeanHeuc's esp32-ldn-civ7](https://github.com/SeanHeuc/CivSwitch/tree/main/esp32-ldn-civ7) (`main.c`, © Sean Heuc) — a port of [CivSwitchLDN](https://github.com/ItsNerisma/CivSwitchLDN) (GPL-3.0-only) and the [Kinnay LDN](https://github.com/kinnay/LDN) key derivation (GPL-3.0) |
| `src/ldn/ldn_civ7.h/.cpp` | GPL-3.0 | Civ VII lobby protocol, ported from CivSwitchLDN via esp32-ldn-civ7 (`civ7.c`, © Sean Heuc) |
| `src/ldn/ldn_crypto.h/.cpp` | GPL-3.0 | Kinnay LDN `KeyDerivation._derive_key` |
| `src/ldn/ldn_install.h/.cpp` | GPL-3.0 | installer streaming, ported from esp32-ldn-civ7 (`main/installer.c`, © Sean Heuc) |
| `src/ldn/ldn_priv.h`, `src/ldn/ldn_hooks.h/.cpp` | AGPL-3.0 | the [pokeldn](https://github.com/Decryptu/pokeldn) `wpa_funcs` handshake-less association patch (ABI restated for the Arduino core's ESP-IDF v5.5.5 prebuilt blobs) |
| `tools/udp_loader.js` | GPL-3.0-only | vendored verbatim from SeanHeuc's esp32-ldn-civ7 (`tools/udp_loader.js`, © Sean Heuc) — the UDP :9027 receiver build_sd.py splices into the chat payload |
| everything else, incl. `src/ldn/ldn.h`, `src/ldn/ldn_keys.h/.cpp` | AGPL-3.0-only | this project |

Credits: **Sean Heuc** (esp32-ldn-civ7 — the LDN flow this firmware ports),
**Gezine** (CivSwitch — payload/installer design, MIT), **ItsNerisma**
(CivSwitchLDN), **Kinnay** (LDN protocol + key derivation), **Decryptu**
(pokeldn).

## Keys policy

Your `/prod.keys` (and anything derived from them) are **never** committed,
baked into a build, or logged: the firmware reads `/prod.keys` from the SD
card at runtime and derives all network keys on-device. Nothing in this
repository contains key material.
