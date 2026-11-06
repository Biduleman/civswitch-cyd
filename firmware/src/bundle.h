/**
 * SD-card bundle access: SEND payload scan + install artifact paths.
 * Bundle layout (produced by tools/build_sd.py): /payloads, /install, /manifest.json.
 */
#pragma once

#include <Arduino.h>
#include <vector>

namespace bundle {

struct PayloadInfo {
  String name; // without .js extension (button label)
  String path; // absolute path on the SD card
  size_t size;
  bool ok;     // false when over the loader's byte limit
};

constexpr const char *PAYLOADS_DIR = "/payloads";
constexpr const char *INSTALL_DIR = "/install";
constexpr const char *STAGE_PATH = "/install/stage.hex";
constexpr const char *TPL_PATH = "/install/tpl_b64.txt";
constexpr const char *APPLY_PATH = "/install/apply.js";
constexpr const char *MANIFEST_PATH = "/install/manifest.json";

/** Scan PAYLOADS_DIR for *.js; appends to `out`. Returns false if missing. */
bool scanPayloads(std::vector<PayloadInfo> &out);

} // namespace bundle
