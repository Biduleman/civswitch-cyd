/**
 * /prod.keys parsing - runtime key material from the SD card.
 *
 * License: AGPL-3.0-only (project license, see the repo-root LICENSE).
 * The required entry names mirror SeanHeuc's tools/make_keys.py. The keys
 * themselves never enter this repo or any build artifact: they live on the
 * user's SD card only and are derived on-device.
 * Ticket 10.
 */
#pragma once

#include <Arduino.h>

namespace ldn {

struct ProdKeys {
  uint8_t kekGen[16];   /* aes_kek_generation_source */
  uint8_t keyGen[16];   /* aes_key_generation_source */
  uint8_t master00[16]; /* master_key_00             */
  uint8_t master12[16]; /* master_key_12             */
};

/** Parses the 4 prod.keys entries make_keys.py requires. Remaps the shared
 * SPI bus to the SD card. Returns false with *err set on failure. */
bool loadProdKeys(const char *path, ProdKeys *out, String *err);

} // namespace ldn
