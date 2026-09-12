#ifndef DES_ECB_H
#define DES_ECB_H

#include <stdint.h>
#include <stddef.h>

// =============================================================================
// Single-block DES decryption, ECB.
//
// WHY THIS EXISTS RATHER THAN A CALL INTO MBEDTLS
//
// mbedtls ships with the ESP32 core and does have DES, but ESP-IDF builds it
// with MBEDTLS_DES_C switched off — DES is deprecated, so it is compiled out of
// the prebuilt library. Calling mbedtls_des_crypt_ecb() therefore compiles
// cleanly and then fails at link with "undefined reference", and there is no way
// to flip that from an Arduino sketch without rebuilding the IDF.
//
// So the algorithm lives here. It is ~120 lines of tables and shifts, it is
// exercised on exactly one thing (unwrapping JioSaavn's `encrypted_media_url`),
// and it is verified against a known ciphertext/plaintext pair before shipping.
//
// This is NOT here to protect anything, and must not be used as though it were.
// DES is broken, the key is a published constant of the service, and the whole
// construction is obfuscation. It is implemented because a URL arrives wrapped
// in it, and for no other reason.
// =============================================================================

/**
 * Decrypt `len` bytes of ECB-mode DES, in place-safe fashion.
 *
 * @param key8  exactly 8 key bytes. Parity bits are ignored, as DES defines.
 * @param in    ciphertext; `len` must be a multiple of 8.
 * @param out   plaintext; may alias `in`.
 * @return false if len is not a positive multiple of 8.
 */
bool desEcbDecrypt(const uint8_t key8[8], const uint8_t* in, uint8_t* out, size_t len);

#endif  // DES_ECB_H
