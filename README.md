# CivSwitch CYD host

An unofficial, third-party Wi-Fi host appliance for
[CivSwitch](https://github.com/Gezine/CivSwitch/) by Gezine (MIT License).
CivSwitch does all the real work — the exploit chain, the Remote JS Loader,
and the savefile installer are entirely Gezine's. This project is only the
little screen that talks to it: a **Cheap Yellow Display (ESP32-2432S028R)**
that replaces the PC/Pi in the CivSwitch workflow as an offline access point,
touch-driven payload sender **and one-tap bootstrapper**.

```
                 Wi-Fi: CivSwitch (AP, NO internet)              USB (data + power)
  Switch ─────────────────────────────────►  CYD (ESP32)   wlan0 = 192.168.4.1
  (loader listens on :9026)                  touch UI: SEND payloads, INSTALL,
                                             BOOTSTRAP (LDN)  │ SD card: payloads
  your PC ◄──────────────────────────────────────────────────┘ + install bundle
```

## Attribution

Built around [CivSwitch](https://github.com/Gezine/CivSwitch/) by
[Gezine](https://github.com/Gezine) (MIT License). CivSwitch does all the
real work — the exploit chain, the Remote JS Loader, and the savefile
installer are entirely Gezine's. The LDN bootstrap ports the local-wireless
flow of [SeanHeuc's esp32-ldn-civ7](https://github.com/SeanHeuc/CivSwitch/tree/main/esp32-ldn-civ7)
(© Sean Heuc) — see [License](#license). This project is only the little
screen that talks to it. Not affiliated with or endorsed by Gezine, SeanHeuc
or Nintendo.

## How it works

- The CYD runs an **offline access point** (`CivSwitch`, no internet — DHCP
  hands out a gateway pointing at the CYD, which drops everything beyond the
  LAN; no DNS). The Switch joins it for CivSwitch's wireless multiplayer.
- The CivSwitch exploit runs **on the Switch** and opens a Remote JS Loader
  on TCP `:9026` (see the CivSwitch repo for the exploit itself).
- The CYD talks to that loader over the network: **SEND** streams raw `.js`
  payloads from the SD card to the loader, and **INSTALL** runs the CivSwitch
  savefile installer (the boot-persistent trigger), driven from the touch UI.
- **BOOTSTRAP (LDN)** removes the last piece of PC/Pi workflow: the CYD joins
  the Civ VII wireless lobby's own local-wireless (LDN) network — scan,
  decrypt the lobby advertisement, derive the network keys from `/prod.keys`
  on the SD card, associate, authenticate — then injects the payload as
  **lobby-chat messages** and streams the installer over UDP. The console
  installs the trigger by itself; the CYD reboots back into AP mode.

## Requirements

- Cheap Yellow Display, ESP32-2432S028R (ILI9341 panel), micro-USB **data**
  cable, FAT32 micro-SD card
- PC with PlatformIO, Node.js and Python for the build steps (`terser` via
  `npm install`)
- A clone of [Gezine/CivSwitch](https://github.com/Gezine/CivSwitch/)
- Your own `/prod.keys` (dumped from **your** Switch 1) on the SD card — for
  the BOOTSTRAP flow only; SEND/INSTALL work without it

## Setup

Expected layout (sibling folders; adjust paths if yours differ):

```
projects/
├── CivSwitch/        ← clone of Gezine/CivSwitch
└── civswitch-cyd/    ← this repo
```

1. **Firmware** — in `firmware/`: `pio run -t upload`, then copy the SD-card
   bundle (step 3) into the slot.
2. **Stage** (compresses the boot payload, PC-side):

   ```sh
   node tools/precompute_stage.js ../CivSwitch/core.js build/stage.hex
   ```

3. **SD bundle**:

   ```sh
   npm install terser          # in tools/ (or in ../CivSwitch/PicoSwitch/)
   python tools/build_sd.py --civswitch ../CivSwitch --stage build/stage.hex --ldn
   ```

   Copy the **contents** of `build/sd/` onto the root of a FAT32 SD card,
   plus your `/prod.keys` at the root. The card ends up with: `payloads/`,
   `install/` (stage, template, installer, chat transport), `manifest.json`.

4. **Switch** — Settings → Internet → join `CivSwitch`
   (passphrase `civswitch7273` by default). Automatic IP works; the appliance
   targets whatever address the Switch gets. The connection test failing is
   expected — there is no internet by design.

## Usage

- **BOOTSTRAP (LDN)** — with a fresh Civ VII lobby open
  (`MULTIPLAYER → WIRELESS → CREATE GAME → HOST LOBBY`), tap it once. The
  screen walks through the phases (scan / join / auth / chat / installer
  chunks); the console ends up with the persistent trigger, the CYD reboots
  back into AP mode. Then on the console: reboot, **Load Game** — the loader
  starts and joins the AP, and SEND/INSTALL just work. **This replaces the
  Pico entirely**: no console settings, no USB typing, ~15 seconds per run.
  A fresh lobby is needed per attempt (the payload replaces the lobby UI).
- **SEND**: tap a payload button → the payload runs on the console, its log
  streams onto the screen (and the full log to the USB serial port).
- **INSTALL**: runs the CivSwitch savefile installer from the SD bundle.
  ⚠️ INSTALL **wipes all single-player saves** and replaces the boot payload —
  a warning with the payload provenance is logged when it runs. Requires the
  console's loader to be running (after a BOOTSTRAP or a one-time Pico
  bootstrap + INSTALL, the loader starts at every boot (Load Game), so the
  button just works from then on).

## Adding payloads

No payload files ship with this repo. The installer bundles `core.js` from
your CivSwitch clone by default; SEND buttons come from `payloads/` in the
same repo. To send your own payloads, drop `.js` files (under 500 KB) in a
folder and pass it at bundle time:

```sh
python tools/build_sd.py --civswitch ../CivSwitch --payloads ../my-payloads --stage build/stage.hex --ldn
```

## Keys policy

Your `/prod.keys` (and anything derived from them) are **never** committed,
baked into a build, or logged: the firmware reads `/prod.keys` from the SD
card at runtime and derives all network keys on-device. Nothing in this
repository contains key material.

## Legal

Provided as-is, for use with hardware and games you own. INSTALL writes to
your console's save data — read the CivSwitch documentation before using it.
You are responsible for complying with the terms that apply to your console.
Not affiliated with or endorsed by Gezine, SeanHeuc or Nintendo.

## AI usage disclosure

This project was developed with the assistance of AI coding agents: the
firmware, PC tooling, and documentation were drafted, iterated, and debugged
in an AI-assisted workflow. Every change was reviewed, accepted, and tested
on real hardware by the maintainer. The underlying CivSwitch project by
Gezine is human-authored and is a separate project.

## License

This project is licensed **AGPL-3.0-only** — see the
[LICENSE](LICENSE) file. The LDN bootstrap ports code from upstream projects
whose licenses apply to those components (notices are preserved in each
file's header):

| Upstream | Author | What was used | License |
|---|---|---|---|
| [esp32-ldn-civ7](https://github.com/SeanHeuc/CivSwitch/tree/main/esp32-ldn-civ7) | Sean Heuc | LDN flow, lobby protocol, installer streaming, UDP payload channel | GPL-3.0 lineage (CivSwitchLDN / Kinnay) |
| [CivSwitchLDN](https://github.com/ItsNerisma/CivSwitchLDN) | ItsNerisma | LDN/Civ VII protocol flow, constants | GPL-3.0-only |
| [Kinnay LDN](https://github.com/kinnay/LDN) | Kinnay | key derivation, advertisement/auth format | GPL-3.0 |
| [pokeldn](https://github.com/Decryptu/pokeldn) | Decryptu | ESP32 `wpa_funcs` handshake-less association patch | AGPL-3.0 |
| [CivSwitch](https://github.com/Gezine/CivSwitch/) | Gezine | payload, save template, installer design | MIT |

The firmware's `src/ldn/` module carries per-file license headers; details
in [firmware/README.md](firmware/README.md) (§ License). Redistribution of a
firmware **binary** requires honoring all of the above.

## Credits

- [CivSwitch](https://github.com/Gezine/CivSwitch/) by Gezine — the exploit,
  loader, installer, and payloads. MIT License.
- [esp32-ldn-civ7](https://github.com/SeanHeuc/CivSwitch/tree/main/esp32-ldn-civ7)
  by Sean Heuc — the local-wireless (LDN) flow the BOOTSTRAP ports.
- [CivSwitchLDN](https://github.com/ItsNerisma/CivSwitchLDN) by ItsNerisma —
  the Civ VII LDN protocol.
- [Kinnay LDN](https://github.com/kinnay/LDN) by Kinnay — LDN protocol and
  key derivation.
- [pokeldn](https://github.com/Decryptu/pokeldn) by Decryptu — the
  handshake-less association patch.
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) and
  [LVGL](https://github.com/lvgl/lvgl) — display and UI.
