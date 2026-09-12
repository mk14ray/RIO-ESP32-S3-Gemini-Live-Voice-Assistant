#ifndef NEWS_H
#define NEWS_H

#include <Arduino.h>

// =============================================================================
// News headlines, resolved entirely on-device via the Google News RSS search
// feed — the same query the browser page at
//
//     https://news.google.com/search?q=<topic>&hl=en-IN&gl=IN&ceid=IN:en
//
// runs, just served as XML instead of a JS-rendered page:
//
//     https://news.google.com/rss/search?q=<topic>&hl=en-IN&gl=IN&ceid=IN:en
//
// One HTTPS GET returns the top results most-relevant-first, each item's
// <title> already in "Headline - Source" form, so no separate source lookup
// is needed.
//
// Mirrors music.cpp's shape and for the same reason: netTask owns the
// WebSocket and must keep polling it, so the TLS lookup runs on its own task.
// The tool call is answered "searching" immediately; the actual headlines (or
// a failure) arrive later as a spoken note through newsTakeNotice().
// =============================================================================

/** Create the request queue and start newsTask. Call once from setup(). */
bool newsBegin();

/**
 * Queue a topic lookup, replacing anything already queued or in flight.
 * Returns false only if the request is unusable or the task is not running.
 * Never blocks.
 */
bool newsRequest(const char* topic);

/** True while a lookup is in progress. */
bool newsIsActive();

/**
 * Collect a pending message for the model — headlines on success, a plain
 * failure reason otherwise; unlike music, this always has something to say
 * once a lookup finishes. Copies at most `cap` bytes and clears the slot.
 *
 * netTask only: it is the sole owner of the WebSocket the text goes out on.
 * @return true if a notice was waiting.
 */
bool newsTakeNotice(char* dst, size_t cap);

#endif  // NEWS_H
