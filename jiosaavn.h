#ifndef JIOSAAVN_H
#define JIOSAAVN_H

#include <Arduino.h>

// =============================================================================
// JioSaavn song lookup, on-device.
//
// Turns a spoken song name into a directly playable audio URL. This replaces
// the old youtube.cpp, which could find a video id and then had nowhere to go
// with it — see the long note in config.h.
//
// The whole resolve is ONE request. MEASURED: search.getResults with n=1
// returns ~34 KB and already carries `encrypted_media_url` for the top hit at
// byte ~677, so the separate song.getDetails round trip the API also offers is
// not needed. 34 KB is small enough to buffer outright, which is why this file
// has no streaming matcher: the top result's fields all land in the first
// kilobyte, so even a truncated read still resolves.
//
// `encrypted_media_url` is base64 wrapping DES-ECB ciphertext under the fixed
// key "38346591". Decrypting is three mbedtls calls against a library already
// linked in for TLS, and yields a plain CDN URL:
//
//     https://aac.saavncdn.com/475/<hash>_96.mp4
//
// MEASURED: that host also answers on plain HTTP (Azure Blob behind it, with
// Accept-Ranges: bytes), so the multi-megabyte transfer costs no second TLS
// session. Only this lookup needs TLS, and it is over in a few tens of KB.
//
// NOT thread-safe: it keeps a file-static receive buffer, and only musicTask
// ever calls it.
// =============================================================================

/**
 * Resolve a song name to a playable audio URL.
 *
 * @param query    song name in plain words, optionally with the artist.
 * @param outUrl   receives the NUL-terminated http:// URL of the audio file.
 * @param urlCap   size of outUrl; JIOSAAVN_URL_MAX is always enough.
 * @param outTitle receives the resolved track title, for logging. May be null.
 * @param titleCap size of outTitle.
 * @param outSecs  receives the track duration in seconds. May be null.
 * @return true only if a playable URL was produced. Blocks for a second or two.
 */
bool jiosaavnResolve(const char* query,
                     char* outUrl,   size_t urlCap,
                     char* outTitle, size_t titleCap,
                     uint32_t* outSecs);

/**
 * Percent-encode `src` into `dst` for use in a query-string value, with spaces
 * written as '+'. Returns false if the result would not fit.
 */
bool jiosaavnUrlEncode(const char* src, char* dst, size_t cap);

#endif  // JIOSAAVN_H
