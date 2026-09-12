#ifndef MEMMON_H
#define MEMMON_H

#include <Arduino.h>

// =============================================================================
// Memory monitoring and admission control.
//
// WHAT THIS IS FOR, AND WHAT IT IS DELIBERATELY NOT FOR
//
// It is not a scheme for freeing the big PSRAM buffers between uses. PSRAM is
// not the scarce resource here — MEASURED on hardware, ~4.8 MB of it sits free
// through an entire session — and churning half-megabyte blocks is what CREATES
// the fragmentation that makes a later camera capture fail. app_state.cpp says
// as much above gImgTxBuf, and it is right: those buffers stay put.
//
// The scarce resource is INTERNAL heap. MEASURED: free internal heap fell to
// 676 bytes while the JioSaavn TLS handshake ran alongside the live Gemini
// session. Nothing crashed that time, but 676 bytes is not headroom, it is luck.
//
// So this file does three things:
//
//   1. Reports the number that actually predicts failure — the LARGEST FREE
//      BLOCK, not the total. A heap with 60 KB free in 200-byte fragments
//      cannot satisfy a 16 KB TLS buffer, and total-free hides that completely.
//
//   2. Admission control: memHaveInternal() lets a caller that is about to do
//      something expensive check first and decline. Declining a song is a small
//      failure the user hears about; running out of heap mid-handshake is a
//      reboot that loses the whole conversation.
//
//   3. Gives every subsystem one consistent way to say what it is holding.
// =============================================================================

/** Free internal (DRAM) bytes right now. */
size_t memFreeInternal();

/**
 * Largest single internal allocation that could succeed right now.
 *
 * This is the figure to watch. Total free can be comfortable while this is far
 * too small, and it is this one that decides whether mbedtls gets its buffers.
 */
size_t memLargestInternal();

/** Lowest free internal heap seen since boot. */
size_t memMinEverInternal();

/**
 * Decide whether an expensive operation should be attempted at all.
 *
 * Checks the largest contiguous block as well as the total, because a request
 * for one big buffer cannot be met out of crumbs. Logs the refusal with the
 * numbers, so a decline is diagnosable rather than mysterious.
 *
 * @param need  bytes the operation is expected to want, at peak.
 * @param what  short label for the log line.
 * @return true if it looks safe to proceed.
 */
bool memHaveInternal(size_t need, const char* what);

/** One-line snapshot, tagged. Cheap; safe to call from any task. */
void memLog(const char* tag);

#endif  // MEMMON_H
