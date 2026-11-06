#!/usr/bin/env python3
"""build_sd.py - build the CYD SD-card bundle for the CivSwitch CYD host.

Needs two things:

1. A clone of CivSwitch (https://github.com/Gezine/CivSwitch/) - supplies the
   save template, the installer constants, and the SEND payloads.
2. A stage produced by tools/precompute_stage.js (the compressed boot
   payload). Build it first:

     node tools/precompute_stage.js ../CivSwitch/core.js build/stage.hex
     python tools/build_sd.py --civswitch ../CivSwitch --stage build/stage.hex

Output: build/sd/  ->  copy the CONTENTS onto the root of a FAT32 SD card.

  payloads/*.js           raw SEND payloads (loader input format: raw JS)
  install/stage.hex       the compressed boot payload (hex transport)
  install/stage.hex.sha256
  install/stage.hex.payload   source payload name, for the INSTALL screen
  install/tpl_b64.txt     save template base64-encoded (exact length matters)
  install/apply.js        the installer payload with stage + template length spliced
  manifest.json           stage provenance for the CYD INSTALL screen

The staleness guard: stage.hex.sha256 is the sha256 of the normalized
payload the stage was built from. build_sd.py re-hashes the payload and
aborts on a mismatch, so a stale stage can never ship.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import sys

LOADER_MAX_BYTES = 500 * 1024


def fail(msg):
    print("E: " + msg)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser(description="build the CYD SD-card bundle")
    ap.add_argument("--civswitch", default="../CivSwitch",
                    help="path to your CivSwitch clone (default: ../CivSwitch)")
    ap.add_argument("--stage", default=os.path.join("build", "stage.hex"),
                    help="precomputed stage hex (default: build/stage.hex)")
    ap.add_argument("--payload", default=None,
                    help="payload the stage was built from, for the staleness check "
                         "(default: <civswitch>/core.js; pass an empty string to skip)")
    ap.add_argument("--payloads", default=None,
                    help="source directory of raw SEND payloads "
                         "(default: <civswitch>/payloads)")
    ap.add_argument("--out", default=os.path.join("build", "sd"),
                    help="output folder to (re)create (default: build/sd)")
    args = ap.parse_args()

    civswitch = os.path.abspath(args.civswitch)
    installer_dir = os.path.join(civswitch, "installer-creator")
    tpl_path = os.path.join(installer_dir, "save_template.bin")
    stage_path = os.path.abspath(args.stage)
    sidecar_path = stage_path + ".sha256"
    name_path = stage_path + ".payload"
    out_dir = os.path.abspath(args.out)
    payload_src = os.path.abspath(args.payloads) if args.payloads \
        else os.path.join(civswitch, "payloads")
    payload_check = args.payload if args.payload is not None \
        else os.path.join(civswitch, "core.js")

    # ---- 0. the CivSwitch clone supplies the installer constants ----
    if not os.path.isfile(os.path.join(installer_dir, "install.py")):
        fail("CivSwitch clone not found at %s\n"
             "       expected installer-creator/install.py - "
             "git clone https://github.com/Gezine/CivSwitch/" % civswitch)
    sys.path.insert(0, installer_dir)
    from install import BLOCK, CHUNK, STAGER  # noqa: E402

    # ---- 1. stage: must be hex AND match its sha256 sidecar (staleness guard)
    if not os.path.exists(stage_path):
        fail("stage file not found: %s\n"
             "       run: node tools/precompute_stage.js <civswitch>/core.js "
             "build/stage.hex" % stage_path)
    stage_hex = open(stage_path, encoding="ascii").read().strip()
    if not re.fullmatch(r"[0-9a-f]+", stage_hex):
        fail("stage file is not a hex string: %s" % stage_path)
    if not os.path.exists(sidecar_path):
        fail("missing %s - rebuild the stage with precompute_stage.js\n"
             "       (this builder refuses an unverifiable stage)" % sidecar_path)
    stage_sha = open(sidecar_path, encoding="ascii").read().strip()
    print("stage: %d chars, sha256 %s" % (len(stage_hex), stage_sha[:16] + "..."))

    if payload_check:
        payload_path = os.path.abspath(payload_check)
        if not os.path.exists(payload_path):
            fail("payload for staleness check not found: %s "
                 "(pass --payload '' to skip)" % payload_path)
        core = open(payload_path, encoding="utf-8").read().replace("\r\n", "\n")
        got = hashlib.sha256(core.encode("utf-8")).hexdigest()
        if got != stage_sha:
            fail("stage was built for a different payload (sha256 mismatch)\n"
                 "       re-run: node tools/precompute_stage.js %s "
                 "build/stage.hex" % payload_path)
        print("staleness: stage matches %s" % os.path.basename(payload_path))

    source_payload = None
    if os.path.exists(name_path):
        source_payload = open(name_path, encoding="ascii").read().strip() or None

    # ---- 2. template: pristine CIV7 save, base64-encoded (binary -> loader text protocol)
    if not os.path.exists(tpl_path):
        fail("save template not found: %s" % tpl_path)
    tpl = open(tpl_path, "rb").read()
    if len(tpl) < 4096 or tpl[:4] != b"CIV7":
        fail("bad save_template.bin (too small or wrong magic)")
    tpl_b64 = base64.b64encode(tpl).decode("ascii")
    chunks = (len(tpl_b64) + CHUNK - 1) // CHUNK
    print("template: %d bytes -> %d b64 chars -> %d chunks of %d" %
          (len(tpl), len(tpl_b64), chunks, CHUNK))

    # ---- 3. apply payload: the exact splice CivSwitch's installer performs
    apply_js = (BLOCK
                .replace("__STAGER__", json.dumps(STAGER))
                .replace("__STAGE_HEX__", json.dumps(stage_hex))
                .replace("__TPL_B64_LEN__", str(len(tpl_b64))))
    for token in ("__STAGER__", "__STAGE_HEX__", "__TPL_B64_LEN__"):
        if token in apply_js:
            fail("internal: placeholder %s was not replaced" % token)

    # ---- 4. SEND payloads: raw .js, within the loader's byte limit
    if not os.path.isdir(payload_src):
        fail("payloads directory not found: %s" % payload_src)
    payload_files = sorted(f for f in os.listdir(payload_src) if f.endswith(".js"))
    if not payload_files:
        fail("no .js payloads found in %s" % payload_src)
    oversized = [f for f in payload_files
                 if os.path.getsize(os.path.join(payload_src, f)) > LOADER_MAX_BYTES]
    if oversized:
        fail("payload(s) over the loader's 500 KB limit: %s" % ", ".join(oversized))

    # ---- 5. write the bundle (fresh)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(os.path.join(out_dir, "install"))
    os.makedirs(os.path.join(out_dir, "payloads"))
    for name in payload_files:
        shutil.copyfile(os.path.join(payload_src, name),
                        os.path.join(out_dir, "payloads", name))
    shutil.copyfile(stage_path, os.path.join(out_dir, "install", "stage.hex"))
    shutil.copyfile(sidecar_path, os.path.join(out_dir, "install", "stage.hex.sha256"))
    if os.path.exists(name_path):
        shutil.copyfile(name_path, os.path.join(out_dir, "install", "stage.hex.payload"))
    with open(os.path.join(out_dir, "install", "tpl_b64.txt"), "w",
              encoding="ascii", newline="") as fh:
        fh.write(tpl_b64)
    with open(os.path.join(out_dir, "install", "apply.js"), "w",
              encoding="ascii", newline="\n") as fh:
        fh.write(apply_js)
    manifest = {
        "source_payload": source_payload,
        "stage_sha256": stage_sha,
        "stage_hex_chars": len(stage_hex),
        "tpl_b64_len": len(tpl_b64),
        "tpl_chunks": chunks,
    }
    with open(os.path.join(out_dir, "manifest.json"), "w",
              encoding="ascii", newline="\n") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")

    print()
    print("bundle written: %s" % out_dir)
    print("copy the CONTENTS of this folder onto the root of a FAT32 SD card.")
    print("boot payload: %s (re-run precompute_stage.js + this script to change it)"
          % (source_payload or "unknown"))


if __name__ == "__main__":
    main()
