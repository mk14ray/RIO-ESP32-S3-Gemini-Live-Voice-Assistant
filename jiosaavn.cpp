#include "jiosaavn.h"
#include "config.h"
#include "secrets.h"
#include "b64.h"

#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include "des_ecb.h"
#include "memmon.h"

// Receive staging. PSRAM and file-static rather than stack: musicTask is the
// only caller (see jiosaavn.h) and JIOSAAVN_RX_MAX does not belong on a stack.
static char* sRx = nullptr;

bool jiosaavnUrlEncode(const char* src, char* dst, size_t cap) {
    if (src == nullptr || dst == nullptr || cap == 0) {
        return false;
    }

    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p; p++) {
        const unsigned char c = *p;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            if (o + 1 >= cap) return false;
            dst[o++] = (char)c;
        } else if (c == ' ') {
            if (o + 1 >= cap) return false;
            dst[o++] = '+';
        } else {
            // Everything else, including every byte of a UTF-8 Devanagari
            // title, goes out as %XX.
            if (o + 3 >= cap) return false;
            // Not named HEX: Arduino's Print.h defines that as the integer 16.
            static const char HEXD[] = "0123456789ABCDEF";
            dst[o++] = '%';
            dst[o++] = HEXD[c >> 4];
            dst[o++] = HEXD[c & 0x0F];
        }
    }

    dst[o] = '\0';
    return true;
}

// -----------------------------------------------------------------------------
// Minimal HTTP/1.1 reader. Same shape as the one music.cpp uses on the CDN
// socket, kept separate only because that one runs over a plain WiFiClient.
// -----------------------------------------------------------------------------
/**
 * Read one CRLF-terminated line into a caller-supplied buffer.
 *
 * A fixed buffer rather than a String on purpose. String grows by reallocating
 * on the INTERNAL heap, once every few characters, and this runs for every
 * header line of every response — precisely the small-block churn that leaves
 * the heap too fragmented for mbedtls to get its next 16 KB buffer. See
 * memmon.h for why internal heap is the resource that matters here.
 *
 * Over-long lines are truncated, not failed: nothing this parses cares about
 * headers beyond the first few characters.
 *
 * @param outLen receives the stored length. May be null.
 */
static bool readLine(WiFiClientSecure& tls, char* out, size_t cap,
                     size_t* outLen, uint32_t deadline) {
    size_t n = 0;
    out[0] = '\0';

    for (;;) {
        if (millis() > deadline) {
            return false;
        }
        if (!tls.available()) {
            if (!tls.connected()) {
                return false;
            }
            delay(2);
            continue;
        }

        const int c = tls.read();
        if (c < 0) {
            continue;
        }
        if (c == '\n') {
            if (n > 0 && out[n - 1] == '\r') {
                n--;
            }
            out[n] = '\0';
            if (outLen != nullptr) *outLen = n;
            return true;
        }
        if (n + 1 < cap) {
            out[n++] = (char)c;
            out[n]   = '\0';
        }
    }
}

/**
 * Copy the value of a JSON string field into dst, undoing the two escapes that
 * actually occur in this response.
 *
 * `\/` is not cosmetic here: `encrypted_media_url` is base64, base64 contains
 * '/', and JSON escapes it. Feeding the raw span to the decoder would fail on
 * every URL that happens to contain one — which is most of them.
 *
 * @param json  the buffer to search, NUL-terminated.
 * @param key   field name including the quotes and colon, e.g. "\"title\":\"".
 */
static bool jsonField(const char* json, const char* key, char* dst, size_t cap) {
    const char* p = strstr(json, key);
    if (p == nullptr) {
        return false;
    }
    p += strlen(key);

    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            const char esc = *p++;
            switch (esc) {
                case '/':  c = '/';  break;
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case 'r':  c = '\r'; break;
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                // \uXXXX and anything else: keep the payload byte-safe by
                // dropping the escape rather than guessing at it. Only the
                // title can contain these, and it is used for logging.
                default:   continue;
            }
        }
        if (o + 1 >= cap) {
            return false;
        }
        dst[o++] = c;
    }

    dst[o] = '\0';
    return o > 0;
}

/**
 * base64 -> DES-ECB decrypt -> printable URL.
 *
 * The key is a constant of the service, not a secret we chose. DES here is
 * obfuscation, not security. It is decrypted by des_ecb.cpp rather than by
 * mbedtls because ESP-IDF compiles MBEDTLS_DES_C out — see the note there.
 */
static bool decryptMediaUrl(const char* enc, char* dst, size_t cap) {
    uint8_t ct[JIOSAAVN_URL_MAX];
    size_t  ctLen = 0;

    if (!b64Decode(enc, strlen(enc), ct, sizeof(ct), &ctLen) || ctLen == 0) {
        Serial.println("[SAAVN] media url is not valid base64");
        return false;
    }
    if (ctLen % 8 != 0) {
        Serial.printf("[SAAVN] ciphertext %u is not a DES block multiple\n",
                      (unsigned)ctLen);
        return false;
    }

    uint8_t pt[JIOSAAVN_URL_MAX];
    if (!desEcbDecrypt((const uint8_t*)JIOSAAVN_DES_KEY, ct, pt, ctLen)) {
        return false;
    }

    // Stop at the first non-printable byte. That covers PKCS#5 padding, zero
    // padding and anything else the service might switch to, without this
    // having to care which one it used: the plaintext is a URL, and a URL is
    // printable ASCII throughout.
    size_t o = 0;
    while (o < ctLen && pt[o] >= 0x20 && pt[o] <= 0x7E && o + 1 < cap) {
        dst[o] = (char)pt[o];
        o++;
    }
    dst[o] = '\0';

    return strncmp(dst, "http", 4) == 0;
}

bool jiosaavnResolve(const char* query,
                     char* outUrl,   size_t urlCap,
                     char* outTitle, size_t titleCap,
                     uint32_t* outSecs) {
    if (query == nullptr || *query == '\0' || outUrl == nullptr || urlCap == 0) {
        return false;
    }
    outUrl[0] = '\0';
    if (outTitle != nullptr && titleCap > 0) outTitle[0] = '\0';
    if (outSecs != nullptr) *outSecs = 0;

    if (sRx == nullptr) {
        sRx = (char*)heap_caps_malloc(JIOSAAVN_RX_MAX, MALLOC_CAP_SPIRAM);
        if (sRx == nullptr) {
            Serial.println("[SAAVN] receive buffer allocation failed");
            return false;
        }
    }

    char enc[MUSIC_QUERY_MAX * 3 + 1];
    if (!jiosaavnUrlEncode(query, enc, sizeof(enc))) {
        Serial.println("[SAAVN] query too long to encode");
        return false;
    }

    // Check before opening a second TLS session against the live Gemini socket.
    // MEASURED, this handshake is what drove free internal heap to 676 bytes;
    // declining a song is a failure the user is told about, running out of heap
    // mid-handshake is a reboot that loses the whole conversation.
    if (!memHaveInternal(MUSIC_LOOKUP_INTERNAL_NEED, "JioSaavn lookup")) {
        return false;
    }

    WiFiClientSecure tls;
    tls.setCACert(DIGICERT_G3_ROOT_PEM);   // www.jiosaavn.com chains to this
    tls.setTimeout(JIOSAAVN_TIMEOUT_MS / 1000);

    const uint32_t deadline = millis() + JIOSAAVN_TIMEOUT_MS;

    if (!tls.connect(JIOSAAVN_HOST, JIOSAAVN_PORT)) {
        Serial.println("[SAAVN] TLS connect to jiosaavn.com failed");
        return false;
    }

    // No Accept-Encoding: identity is what we want, for the same reason
    // youtube.cpp wanted it — nothing here can inflate gzip. At ~34 KB the
    // saving would not be worth a decompressor anyway.
    char req[512];
    const int reqLen = snprintf(req, sizeof(req),
        "GET " JIOSAAVN_API_PATH "%s HTTP/1.1\r\n"
        "Host: " JIOSAAVN_HOST "\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0 Safari/537.36\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "\r\n", enc);
    if (reqLen <= 0 || (size_t)reqLen >= sizeof(req)) {
        Serial.println("[SAAVN] request did not fit");
        tls.stop();
        return false;
    }

    if (tls.print(req) == 0) {
        Serial.println("[SAAVN] failed to send search request");
        tls.stop();
        return false;
    }

    // --- Status line + headers -----------------------------------------------
    char   line[256];
    size_t lineLen = 0;

    if (!readLine(tls, line, sizeof(line), &lineLen, deadline)) {
        Serial.println("[SAAVN] no response to search request");
        tls.stop();
        return false;
    }

    const char* sp = strchr(line, ' ');
    const int status = (sp != nullptr) ? atoi(sp + 1) : 0;
    if (status != 200) {
        Serial.printf("[SAAVN] search returned HTTP %d\n", status);
        tls.stop();
        return false;
    }

    // Headers are skipped rather than examined. Transfer-Encoding does not need
    // detecting here: chunk size lines are hex digits and CRLFs spliced between
    // JSON spans, and they cannot appear inside a quoted value nor forge a match
    // for a key that carries its own quotes and colon. See the body note below.
    for (;;) {
        if (!readLine(tls, line, sizeof(line), &lineLen, deadline)) {
            Serial.println("[SAAVN] truncated headers");
            tls.stop();
            return false;
        }
        if (lineLen == 0) {
            break;
        }
    }

    // --- Body ----------------------------------------------------------------
    // Read until the socket closes or the buffer fills, then NUL-terminate and
    // work on it as a string. A truncated read is not fatal and is not even
    // unusual: the top result's fields sit in the first kilobyte, and this only
    // ever reports the top result.
    //
    // Chunk size lines are left in place deliberately. They are hex digits and
    // CRLFs spliced between JSON spans, which cannot appear inside the quoted
    // values being extracted and cannot create a false match for a key that
    // includes its own quotes and colon.
    size_t n = 0;
    uint32_t idleSince = millis();

    while (n + 1 < JIOSAAVN_RX_MAX && millis() < deadline) {
        const int avail = tls.available();
        if (avail <= 0) {
            if (!tls.connected()) {
                break;
            }
            if (millis() - idleSince > 5000) {
                break;
            }
            delay(2);
            continue;
        }

        size_t want = JIOSAAVN_RX_MAX - 1 - n;
        if ((size_t)avail < want) {
            want = (size_t)avail;
        }

        const int got = tls.read((uint8_t*)sRx + n, want);
        if (got <= 0) {
            delay(2);
            continue;
        }
        n += (size_t)got;
        idleSince = millis();
    }

    tls.stop();
    sRx[n] = '\0';

    if (n == 0) {
        Serial.println("[SAAVN] empty search response");
        return false;
    }

    // --- Extract -------------------------------------------------------------
    char encUrl[JIOSAAVN_URL_MAX * 2];
    if (!jsonField(sRx, "\"encrypted_media_url\":\"", encUrl, sizeof(encUrl))) {
        Serial.printf("[SAAVN] no playable result for \"%s\" (%u bytes scanned)\n",
                      query, (unsigned)n);
        return false;
    }

    if (outTitle != nullptr && titleCap > 0) {
        jsonField(sRx, "\"title\":\"", outTitle, titleCap);
    }
    if (outSecs != nullptr) {
        char dur[16];
        if (jsonField(sRx, "\"duration\":\"", dur, sizeof(dur))) {
            *outSecs = (uint32_t)atoi(dur);
        }
    }

    char url[JIOSAAVN_URL_MAX];
    if (!decryptMediaUrl(encUrl, url, sizeof(url))) {
        Serial.println("[SAAVN] media url did not decrypt to a URL");
        return false;
    }

    // The service hands back https. Rewrite to http on purpose: MEASURED, the
    // CDN answers identically on port 80, and skipping TLS on a multi-megabyte
    // transfer avoids a second mbedtls session against the live Gemini socket.
    // The payload is public audio; there is nothing here worth a handshake.
    const char* body = url;
    if (strncmp(url, "https://", 8) == 0) {
        body = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        body = url + 7;
    }

    // Swap the bitrate suffix the service chose for the one config.h asks for.
    // Every quality is plain AAC-LC (MEASURED: _12 _48 _96 _160 _320 all report
    // audioObjectType 2), so this only changes bandwidth and sample rate.
    char stem[JIOSAAVN_URL_MAX];
    snprintf(stem, sizeof(stem), "%s", body);
    char* us = strrchr(stem, '_');
    char* dot = strrchr(stem, '.');
    if (us != nullptr && dot != nullptr && dot > us) {
        *us = '\0';
        snprintf(outUrl, urlCap, "http://%s%s%s", stem, JIOSAAVN_QUALITY, dot);
    } else {
        snprintf(outUrl, urlCap, "http://%s", body);
    }

    return true;
}
