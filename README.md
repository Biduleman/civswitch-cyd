# CivSwitch CYD host

An unofficial, third-party Wi-Fi host appliance for
[CivSwitch](https://github.com/Gezine/CivSwitch/) by Gezine (MIT License).
CivSwitch does all the real work — the exploit chain, the Remote JS Loader,
and the savefile installer are entirely Gezine's. This project is only the
little screen that talks to it: a **Cheap Yellow Display (ESP32-2432S028R)**
that replaces the PC/Pi in the CivSwitch workflow as an offline access point
and touch-driven payload sender.

```
                 Wi-Fi: CivSwitch (AP, NO internet)              USB (data + power)
  Switch ─────────────────────────────────►  CYD (ESP32)   wlan0 = 192.168.4.1
  (loader listens on :9026)                  touch UI: SEND payloads, INSTALL
                                             SD card: payloads + install bundle │ USB
  your PC ◄────────────────────────────────────────────────────────────────────────┘
```

## Attribution

Built around [CivSwitch](https://github.com/Gezine/CivSwitch/) by
[Gezine](https://github.com/Gezine) (MIT License). CivSwitch does all the
real work — the exploit chain, the Remote JS Loader, and the savefile
installer are entirely Gezine's. This project is only the little screen that
talks to it. Not affiliated with or endorsed by Gezine or Nintendo.

## How it works

- The CYD runs an **offline access point** (`CivSwitch`, no internet — DHCP
  hands out a gateway pointing at the CYD, which drops everything beyond the
  LAN; no DNS). The Switch joins it for CivSwitch's wireless multiplayer.
- The CivSwitch exploit runs **on the Switch** and opens a Remote JS Loader
  on TCP `:9026` (see the CivSwitch repo for the exploit itself and the
  one-time Pico bootstrap).
- The CYD talks to that loader over the network: **SEND** streams raw `.js`
  payloads from the SD card to the loader, and **INSTALL** runs the CivSwitch
  savefile installer (the boot-persistent trigger), driven from the touch UI.

It does **not** replace the Pico (which types the one-time bootstrap into the
console) or the PC-side build steps — it replaces only the network host.

## Requirements

- Cheap Yellow Display, ESP32-2432S028R (ILI9341 panel), micro-USB **data**
  cable, FAT32 micro-SD card
- PC with PlatformIO, Node.js and Python for the build steps
- A clone of [Gezine/CivSwitch](https://github.com/Gezine/CivSwitch/)
- The Civ7 cartridge + a flashed Pico for the one-time console bootstrap
  (see the CivSwitch repo)

## Setup

Expected layout (sibling folders; adjust paths if yours differ):

```
projects/
├── CivSwitch/        ← clone of Gezine/CivSwitch
└── civswitch-cyd/    ← this repo
```

1. **Firmware** — in `firmware/`: `pio run -t upload`, then copy the SD-card
   bundle (step 4) into the slot.
2. **Stage** (compresses the boot payload, PC-side):

   ```sh
   node tools/precompute_stage.js ../CivSwitch/core.js build/stage.hex
   ```

3. **SD bundle**:

   ```sh
   python tools/build_sd.py --civswitch ../CivSwitch --stage build/stage.hex
   ```

   Copy the **contents** of `build/sd/` onto the root of a FAT32 SD card:
   `payloads/`, `install/`, `manifest.json`.

4. **Switch** — Settings → Internet → join `CivSwitch`
   (passphrase `civswitch7273` by default). Automatic IP works; the appliance
   targets whatever address the Switch gets. The connection test failing is
   expected — there is no internet by design.

## Usage

- **SEND**: tap a payload button → the payload runs on the console, its log
  streams onto the screen (and the full log to the USB serial port).
- **INSTALL**: runs the CivSwitch savefile installer from the SD bundle.
  ⚠️ INSTALL **wipes all single-player saves** and replaces the boot payload —
  it asks for a 2-second hold first. Requires the console's loader to be
  running: after the one-time Pico bootstrap + INSTALL, the loader starts at
  every boot (Load Game), so the button just works from then on.

## Adding payloads

No payload files ship with this repo. The installer bundles `core.js` from
your CivSwitch clone by default; SEND buttons come from `payloads/` in the
same repo. To send your own payloads, drop `.js` files (under 500 KB) in a
folder and pass it at bundle time:

```sh
python tools/build_sd.py --civswitch ../CivSwitch --payloads ../CivSwitch/payloads --payloads-extra ../my-payloads
```

## Legal

Provided as-is, for use with hardware and games you own. INSTALL writes to
your console's save data — read the CivSwitch documentation before using it.
You are responsible for complying with the terms that apply to your console.
Not affiliated with or endorsed by Gezine or Nintendo.

## AI usage disclosure

This project was developed with the assistance of AI coding agents: the
firmware, PC tooling, and documentation were drafted, iterated, and debugged
in an AI-assisted workflow. Every change was reviewed, accepted, and tested
on real hardware by the maintainer. The underlying CivSwitch project by
Gezine is human-authored and is a separate project.

## Credits

- [CivSwitch](https://github.com/Gezine/CivSwitch/) by Gezine — the exploit,
  loader, installer, and payloads. MIT License.
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) and
  [LVGL](https://github.com/lvgl/lvgl) — display and UI.
