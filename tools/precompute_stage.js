#!/usr/bin/env node
// precompute_stage.js - compress a payload into stage.hex ONCE, on a machine
// that has Node (your PC). The stage is an input to tools/build_sd.py, which
// produces the SD-card bundle for the CYD host.
//
// usage:
//   node tools/precompute_stage.js --civswitch ../CivSwitch [options]
//
// options:
//   --civswitch <path>   your CivSwitch clone (default: ../CivSwitch)
//   --payload <file.js>  payload to stage (default: <civswitch>/core.js)
//   --out <file.hex>     stage output (default: build/stage.hex)
//
// Runs the CivSwitch repo's own installer-creator/compress_stage.js from your
// clone (nothing from CivSwitch is copied into this repo). Requires
// `npm install` in <civswitch>/installer-creator (terser) - that is also what
// CivSwitch's installer needs.
//
// Produces <out.hex> with exactly the bytes CivSwitch's installer step would
// produce: same payload wrap, same compression, same offline EXACT round-trip
// check. Also writes <out.hex>.sha256 (sha256 of the normalized payload) and
// <out.hex>.payload (source payload name) so the bundle builder can detect a
// stale stage and the CYD can show provenance.

const fs = require("fs"),
  os = require("os"),
  path = require("path"),
  crypto = require("crypto");
const { execFileSync } = require("child_process");

let civswitch = "../CivSwitch";
let payload = null;
let outHex = path.join("build", "stage.hex");

const argv = process.argv.slice(2);
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === "--civswitch") civswitch = argv[++i];
  else if (a === "--payload") payload = argv[++i];
  else if (a === "--out") outHex = argv[++i];
  else if (a.endsWith(".hex") && !payload) outHex = a; // backwards-friendly
  else payload = a;
}
if (!payload) payload = path.join(civswitch, "core.js");

const core = fs.readFileSync(payload, "utf8").replace(/\r\n/g, "\n");
const wrapped =
  "(async function() {\ntry{\n" +
  core +
  "\n} catch (e) {\nawait log('EXCEPTION: ' + e.message);\nawait log(e.stack);\n}\n})();";

const civswitchDir = path.resolve(civswitch);
const compressStage = path.join(civswitchDir, "installer-creator", "compress_stage.js");
if (!fs.existsSync(compressStage)) {
  console.error(
    "compress_stage.js not found at " + compressStage +
    "\n--civswitch must point at a CivSwitch clone "
    + "(git clone https://github.com/Gezine/CivSwitch/)"
  );
  process.exit(1);
}
if (!fs.existsSync(path.join(civswitchDir, "installer-creator", "node_modules", "terser"))) {
  console.error(
    "terser not found - run:  cd " + path.join(civswitchDir, "installer-creator") +
    "  &&  npm install"
  );
  process.exit(1);
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "civ7stage_"));
fs.mkdirSync(path.dirname(path.resolve(outHex)), { recursive: true });
try {
  const wrappedPath = path.join(tmp, "wrapped.js");
  fs.writeFileSync(wrappedPath, wrapped, "utf8");
  execFileSync(
    process.execPath,
    [compressStage, wrappedPath, outHex],
    { stdio: "inherit" }
  );
} finally {
  fs.rmSync(tmp, { recursive: true, force: true });
}

const digest = crypto
  .createHash("sha256")
  .update(Buffer.from(core, "utf8"))
  .digest("hex");
fs.writeFileSync(outHex + ".sha256", digest + "\n", "utf8");
fs.writeFileSync(outHex + ".payload", path.basename(payload) + "\n", "utf8");
console.log("stage written: " + outHex);
console.log("payload sha256: " + digest);
console.log(
  "next: python tools/build_sd.py --civswitch " + civswitch + " --stage " + outHex
);
console.log("re-run this whenever the payload changes - the bundle builder refuses a stale stage.");
