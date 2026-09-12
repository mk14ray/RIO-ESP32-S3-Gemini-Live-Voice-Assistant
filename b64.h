#ifndef B64_H
#define B64_H

#include <Arduino.h>

// Thin wrappers over mbedtls base64 (ships with the ESP32 core).
//
// Sizing, both directions:
//   encode: 4 * ((rawLen + 2) / 3) + 1     (+1 for the NUL mbedtls always writes)
//   decode: 3 * (b64Len / 4)               (upper bound; '=' padding yields less)

/** Buffer size required by b64Encode, including the trailing NUL. */
size_t b64EncodedLen(size_t rawLen);

/** Upper bound on bytes produced by b64Decode. */
size_t b64MaxDecodedLen(size_t b64Len);

/**
 * Base64-encode src into dst. dst is NUL-terminated on success.
 * @param olen receives the encoded length excluding the NUL.
 */
bool b64Encode(const uint8_t* src, size_t slen, char* dst, size_t dcap, size_t* olen);

/**
 * Base64-decode slen chars of src into dst.
 * mbedtls rejects any character outside the base64 alphabet (bar \r\n), which
 * doubles as a check that the span we extracted was really base64.
 */
bool b64Decode(const char* src, size_t slen, uint8_t* dst, size_t dcap, size_t* olen);

#endif  // B64_H
