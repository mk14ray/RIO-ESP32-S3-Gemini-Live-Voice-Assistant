#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <Arduino.h>

/** Start the WiFi connection. Blocks until associated or the timeout expires. */
bool wifiBegin(uint32_t timeoutMs = 20000);

/**
 * Called periodically from loop(). Re-associates with exponential backoff
 * (1 -> 30 s) if the link has dropped.
 */
void wifiEnsure();

bool wifiIsConnected();

/** Current RSSI in dBm, or 0 if not connected. */
int wifiRssi();

/**
 * Start the SNTP client and set the local timezone to NTP_TZ. Returns
 * immediately: the first sync lands asynchronously a second or two later, and
 * the client re-syncs on its own from then on. Safe to call before the link is
 * up — lwIP retries until it answers.
 */
void timeSyncBegin();

/** True once the clock holds a plausible wall time rather than the 1970 boot value. */
bool timeIsSynced();

/**
 * Block until the clock syncs or `timeoutMs` elapses. Returns true if synced.
 * Call once at boot so the first Gemini session can carry a real date; returning
 * false is survivable, not fatal.
 */
bool timeWaitSync(uint32_t timeoutMs);

/**
 * Format the current local (IST) time into `out` using TIME_PROMPT_FORMAT.
 * Returns false and leaves `out` empty if the clock has not synced yet, so
 * callers can omit the time entirely instead of stating a wrong one.
 */
bool timeNowLocal(char* out, size_t n);

#endif  // WIFI_MGR_H
