/**
 * Crypto primitives for the Civ VII LDN stack: SHA-256, AES-ECB decrypt,
 * AES-CTR, HMAC-SHA256, and Kinnay's KeyDerivation._derive_key fed from the
 * runtime /prod.keys (ldn_keys.h).
 *
 * SPDX-License-Identifier: GPL-3.0
 * The derivation algorithm is Kinnay's LDN implementation
 * (https://github.com/kinnay/LDN, GPL-3.0) via SeanHeuc's esp32-ldn-civ7.
 * The crypto calls themselves go to mbedtls (Apache-2.0, part of the
 * Arduino core). Ticket 10.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ldn_keys.h"

namespace ldn {
namespace crypto {

void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

void aesEcbDec(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/** In-place AES-CTR over data[0..len); 4-byte nonce, counter starts at 0
 * (nonce || 12 zero bytes). */
void aesCtr(const uint8_t key[16], const uint8_t nonce4[4], uint8_t *data,
            size_t len);

/** 0 on success. */
int hmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *data,
               size_t len, uint8_t out[32]);

/** Kinnay KeyDerivation._derive_key:
 *   kek = AES-ECB-dec(AES-ECB-dec(AES-ECB-dec(master00, kekGen), source), keyGen)
 *   out = AES-ECB-dec(kek, SHA256(data)[:16])                                   */
void deriveKey(const ProdKeys &keys, const uint8_t *data, size_t dataLen,
               const uint8_t source[16], uint8_t out[16]);

} // namespace crypto
} // namespace ldn
