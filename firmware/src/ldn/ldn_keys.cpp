/*
 * /prod.keys parser. License: AGPL-3.0-only (project license, see the
 * repo-root LICENSE); the file format + entry names follow SeanHeuc's
 * tools/make_keys.py (sha256-of-name style "name = hex"). Ticket 10.
 */
#include "ldn_keys.h"

#include <SD.h>

#include "../hw.h"

namespace ldn {

namespace {

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseHex16(const String &value, uint8_t *out) {
  String packed;
  for (size_t i = 0; i < value.length(); i++)
    if (value[i] != ' ') packed += value[i];
  if (packed.length() != 32) return false;
  for (int i = 0; i < 16; i++) {
    int hi = hexVal(packed[2 * i]);
    int lo = hexVal(packed[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

} // namespace

bool loadProdKeys(const char *path, ProdKeys *out, String *err) {
  struct Entry {
    const char *name;
    uint8_t *dst;
    bool found;
  };
  Entry entries[4] = {
      {"aes_kek_generation_source", out->kekGen, false},
      {"aes_key_generation_source", out->keyGen, false},
      {"master_key_00", out->master00, false},
      {"master_key_12", out->master12, false},
  };

  hw::busForSd();
  File f = SD.open(path, FILE_READ);
  if (!f) {
    if (err) *err = String("cannot open ") + path;
    return false;
  }
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line[0] == '#') continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String name = line.substring(0, eq);
    String value = line.substring(eq + 1);
    name.trim();
    value.trim();
    for (int i = 0; i < 4; i++) {
      if (name == entries[i].name) {
        if (!parseHex16(value, entries[i].dst)) {
          f.close();
          if (err) *err = String(entries[i].name) + " must be 16 bytes of hex";
          return false;
        }
        entries[i].found = true;
      }
    }
  }
  f.close();
  for (int i = 0; i < 4; i++) {
    if (!entries[i].found) {
      if (err) *err = String(path) + " is missing " + entries[i].name;
      return false;
    }
  }
  return true;
}

} // namespace ldn
