/*
 * AES/SHA/HMAC + Kinnay key derivation for the LDN stack.
 * SPDX-License-Identifier: GPL-3.0
 * The derivation algorithm is Kinnay's LDN implementation
 * (https://github.com/kinnay/LDN, GPL-3.0) via SeanHeuc's esp32-ldn-civ7;
 * mbedtls (Apache-2.0) provides the primitives. Ticket 10.
 */
#include "ldn_crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

namespace ldn {
namespace crypto {

void sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  mbedtls_sha256(data, len, out, 0);
}

void aesEcbDec(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

void aesCtr(const uint8_t key[16], const uint8_t nonce4[4], uint8_t *data,
            size_t len) {
  mbedtls_aes_context ctx;
  uint8_t nonce[16] = {0};
  uint8_t stream[16] = {0};
  size_t off = 0;
  memcpy(nonce, nonce4, 4); /* nonce(4) + counter(12, from 0) */
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ctr(&ctx, len, &off, nonce, stream, data, data);
  mbedtls_aes_free(&ctx);
}

int hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data,
               size_t len, uint8_t out[32]) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return -1;
  return mbedtls_md_hmac(md, key, keyLen, data, len, out);
}

void deriveKey(const ProdKeys &keys, const uint8_t *data, size_t dataLen,
               const uint8_t source[16], uint8_t out[16]) {
  uint8_t kek[16], digest[32];
  aesEcbDec(keys.master00, keys.kekGen, kek); /* AES-ECB-dec(kek_source, master) */
  aesEcbDec(kek, source, kek);               /* AES-ECB-dec(source, kek)        */
  aesEcbDec(kek, keys.keyGen, kek);          /* AES-ECB-dec(key_source, kek)    */
  sha256(data, dataLen, digest);
  aesEcbDec(kek, digest, out); /* AES-ECB-dec(SHA256(data)[:16]) */
}

} // namespace crypto
} // namespace ldn
