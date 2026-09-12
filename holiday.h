#ifndef HOLIDAY_H
#define HOLIDAY_H

#include <Arduino.h>

// =============================================================================
// Public/state holiday lookup, resolved entirely on-device via Simpliance's
// per-state labour-law holiday list:
//
//     https://www.simpliance.in/India/LEI/holiday_list/<state-slug>/<year>
//
// This is what actually stands in for india.gov.in/calendar (and
// /calendar/bihar): that portal is a client-rendered Next.js app with no data
// reachable by a plain HTTPS GET — the initial HTML carries zero holiday text,
// only a JS shell that fetches the list after the page loads in a browser (see
// news.h for the same "page vs. real feed" problem, solved differently, for
// news). Simpliance's equivalent page is genuinely server-rendered: each row
// of its holiday table carries data-holiday-name/date/type attributes in the
// plain HTML, which is what makes it fetchable by firmware at all.
//
// The table lists a whole calendar year, chronologically, and the rows this
// cares about can sit deep in the page (MEASURED: past 175 KB on a ~250 KB
// page), so unlike news.cpp this buffers the response in full rather than
// stopping early — see HOLIDAY_RX_MAX in config.h.
//
// Mirrors music.cpp/news.cpp's shape otherwise: netTask owns the WebSocket and
// must keep polling it, so the fetch runs on its own task. The tool call is
// answered "searching" immediately; the actual holidays (or a failure) arrive
// later through holidayTakeNotice().
// =============================================================================

/** Create the request queue and start holidayTask. Call once from setup(). */
bool holidayBegin();

/**
 * Queue a lookup for the given Indian state, replacing anything already
 * queued or in flight. Pass an empty string (or nullptr) to use
 * HOLIDAY_DEFAULT_STATE. Never blocks.
 */
bool holidayRequest(const char* state);

/** True while a lookup is in progress. */
bool holidayIsActive();

/**
 * Collect a pending message for the model — the next few upcoming holidays on
 * success, a plain failure reason otherwise; like news, this always has
 * something to say once a lookup finishes. Copies at most `cap` bytes and
 * clears the slot.
 *
 * netTask only: it is the sole owner of the WebSocket the text goes out on.
 * @return true if a notice was waiting.
 */
bool holidayTakeNotice(char* dst, size_t cap);

#endif  // HOLIDAY_H
