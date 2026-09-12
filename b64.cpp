#include "b64.h"
#include <mbedtls/base64.h>

size_t b64EncodedLen(size_t rawLen) {
    return 4 * ((rawLen + 2) / 3) + 1;
}

size_t b64MaxDecodedLen(size_t b64Len) {
    return 3 * (b64Len / 4);
}

bool b64Encode(const uint8_t* src, size_t slen, char* dst, size_t dcap, size_t* olen) {
    if (src == nullptr || dst == nullptr || olen == nullptr) {
        return false;
    }

    size_t written = 0;
    int rc = mbedtls_base64_encode((unsigned char*)dst, dcap, &written, src, slen);
    if (rc != 0) {
        // MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL leaves the required size in `written`.
        Serial.printf("[B64] encode failed rc=-0x%04X (need %u, have %u)\n",
                      -rc, (unsigned)written, (unsigned)dcap);
        return false;
    }

    *olen = written;
    return true;
}

bool b64Decode(const char* src, size_t slen, uint8_t* dst, size_t dcap, size_t* olen) {
    if (src == nullptr || dst == nullptr || olen == nullptr) {
        return false;
    }

    size_t written = 0;
    int rc = mbedtls_base64_decode(dst, dcap, &written, (const unsigned char*)src, slen);
    if (rc != 0) {
        Serial.printf("[B64] decode failed rc=-0x%04X (in %u, cap %u)\n",
                      -rc, (unsigned)slen, (unsigned)dcap);
        return false;
    }

    *olen = written;
    return true;
}
