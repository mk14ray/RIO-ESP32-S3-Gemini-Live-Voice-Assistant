#include "holiday.h"
#include "config.h"
#include "secrets.h"
#include "memmon.h"
#include "wifi_mgr.h"

#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

typedef struct {
    char state[HOLIDAY_STATE_MAX];
} HolidayReq;

static QueueHandle_t sReqQ = NULL;
static TaskHandle_t  sTask = NULL;

// Receive staging. PSRAM and file-static rather than stack, same reasoning as
// jiosaavn.cpp's sRx: holidayTask is the only caller and HOLIDAY_RX_MAX does
// not belong on a stack.
static char* sRx = nullptr;

static std::atomic<bool> sActive(false);

static char               sNotice[HOLIDAYS_TEXT_MAX + 96];
static std::atomic<bool>  sNoticeReady(false);

static void postNotice(const char* fmt, ...) {
    if (sNoticeReady.load()) {
        return;                 // an older, equally true result is still queued
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sNotice, sizeof(sNotice), fmt, ap);
    va_end(ap);
    sNoticeReady.store(true);
}

bool holidayIsActive() {
    return sActive.load();
}

bool holidayTakeNotice(char* dst, size_t cap) {
    if (!sNoticeReady.load() || dst == nullptr || cap == 0) {
        return false;
    }
    snprintf(dst, cap, "%s", sNotice);
    sNoticeReady.store(false);
    return true;
}

/**
 * Lowercase and hyphenate a state name into the slug Simpliance's URL expects
 * — MEASURED against the live site: "Uttar Pradesh" -> "uttar-pradesh",
 * "Jammu and Kashmir" -> "jammu-and-kashmir". Letters and digits pass through
 * lowercased; runs of anything else (spaces, punctuation) collapse to one '-'.
 */
static bool slugify(const char* src, char* dst, size_t cap) {
    size_t o = 0;
    bool   pendingDash = false;

    for (const unsigned char* p = (const unsigned char*)src; *p; p++) {
        if (isalnum(*p)) {
            if (pendingDash && o > 0) {
                if (o + 1 >= cap) return false;
                dst[o++] = '-';
            }
            pendingDash = false;
            if (o + 1 >= cap) return false;
            dst[o++] = (char)tolower(*p);
        } else {
            pendingDash = true;   // collapse into the next separator, if any
        }
    }
    dst[o] = '\0';
    return o > 0;
}

/** Read one CRLF-terminated line. Same shape as jiosaavn.cpp / news.cpp. */
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

/** Decode the handful of HTML entities that can appear in a holiday name. */
static void decodeHtmlEntities(char* s) {
    char* r = s;
    char* w = s;
    while (*r) {
        if (*r == '&') {
            if      (strncmp(r, "&amp;",  5) == 0) { *w++ = '&';  r += 5; continue; }
            if      (strncmp(r, "&lt;",   4) == 0) { *w++ = '<';  r += 4; continue; }
            if      (strncmp(r, "&gt;",   4) == 0) { *w++ = '>';  r += 4; continue; }
            if      (strncmp(r, "&quot;", 6) == 0) { *w++ = '"';  r += 6; continue; }
            if      (strncmp(r, "&#39;",  5) == 0) { *w++ = '\''; r += 5; continue; }
            if      (strncmp(r, "&apos;", 6) == 0) { *w++ = '\''; r += 6; continue; }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/** Parse a "YYYY-MM-DD" attribute value. Returns false if it doesn't fit that shape. */
static bool parseIsoDate(const char* s, size_t len, int* y, int* m, int* d) {
    if (len != 10 || s[4] != '-' || s[7] != '-') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (i == 4 || i == 7) continue;
        if (!isdigit((unsigned char)s[i])) return false;
    }
    *y = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    *m = (s[5]-'0')*10 + (s[6]-'0');
    *d = (s[8]-'0')*10 + (s[9]-'0');
    return true;
}

static const char* const MONTH_ABBR[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/**
 * Pull the next `maxResults` holidays on or after (todayY,todayM,todayD) out
 * of the page, joined "Name (DD Mon); Name (DD Mon)" into `out`.
 *
 * strstr is safe here (unlike gemini_live.cpp's WebSocket scan) because sRx is
 * always NUL-terminated after the read loop below.
 *
 * Each row's data-holiday-type is skipped when it reads "Optional" — the same
 * distinction india.gov.in's own legend draws between Gazetted and Restricted
 * holidays: this only speaks the ones the state actually observes.
 *
 * The table is already in chronological order, so once `maxResults` entries
 * are collected nothing later in the page could be sooner — the scan stops
 * there rather than reading through the rest of the year.
 *
 * @return the number of holidays found.
 */
static int extractUpcoming(const char* html, int todayY, int todayM, int todayD,
                           char* out, size_t outCap, int maxResults) {
    int    count   = 0;
    size_t written = 0;
    out[0] = '\0';

#define HOLIDAY_ATTR_NAME "data-holiday-name=\""
#define HOLIDAY_ATTR_DATE "data-holiday-date=\""
#define HOLIDAY_ATTR_TYPE "data-holiday-type=\""

    const char* p = html;
    while (count < maxResults) {
        const char* nameTag = strstr(p, HOLIDAY_ATTR_NAME);
        if (nameTag == nullptr) break;
        const char* nameStart = nameTag + (sizeof(HOLIDAY_ATTR_NAME) - 1);
        const char* nameEnd   = strchr(nameStart, '"');
        if (nameEnd == nullptr) break;

        const char* dateTag = strstr(nameEnd, HOLIDAY_ATTR_DATE);
        if (dateTag == nullptr) break;
        const char* dateStart = dateTag + (sizeof(HOLIDAY_ATTR_DATE) - 1);
        const char* dateEnd   = strchr(dateStart, '"');
        if (dateEnd == nullptr) break;

        const char* typeTag = strstr(dateEnd, HOLIDAY_ATTR_TYPE);
        if (typeTag == nullptr) break;
        const char* typeStart = typeTag + (sizeof(HOLIDAY_ATTR_TYPE) - 1);
        const char* typeEnd   = strchr(typeStart, '"');
        if (typeEnd == nullptr) break;

        p = typeEnd + 1;   // advance past this row regardless of what follows

        int y = 0, m = 0, d = 0;
        const bool isOptional = (size_t)(typeEnd - typeStart) == 8
                               && strncmp(typeStart, "Optional", 8) == 0;

        if (!parseIsoDate(dateStart, (size_t)(dateEnd - dateStart), &y, &m, &d)
            || isOptional) {
            continue;
        }

        const bool isPast = (y < todayY)
                          || (y == todayY && m < todayM)
                          || (y == todayY && m == todayM && d < todayD);
        if (isPast) {
            continue;
        }

        char name[HOLIDAY_NAME_MAX];
        size_t nlen = (size_t)(nameEnd - nameStart);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, nameStart, nlen);
        name[nlen] = '\0';
        decodeHtmlEntities(name);

        const char* mon = (m >= 1 && m <= 12) ? MONTH_ABBR[m - 1] : "?";
        const int n = snprintf(out + written, outCap - written, "%s%s (%d %s)",
                               (count > 0) ? "; " : "", name, d, mon);
        if (n < 0 || (size_t)n >= outCap - written) {
            break;   // no more room; keep what was already collected
        }
        written += (size_t)n;
        count++;
    }
    return count;
}
#undef HOLIDAY_ATTR_NAME
#undef HOLIDAY_ATTR_DATE
#undef HOLIDAY_ATTR_TYPE

/** Fetch one year's page for `slug` into sRx. Returns false on any transport failure. */
static bool fetchYear(const char* slug, int year) {
    char path[128];
    const int pn = snprintf(path, sizeof(path), "%s%s/%d",
                            HOLIDAY_PATH_PREFIX, slug, year);
    if (pn <= 0 || (size_t)pn >= sizeof(path)) {
        return false;
    }

    // Checked before opening a second TLS session against the live Gemini
    // socket, same reasoning as MUSIC_LOOKUP_INTERNAL_NEED.
    if (!memHaveInternal(HOLIDAY_LOOKUP_INTERNAL_NEED, "holiday lookup")) {
        return false;
    }

    WiFiClientSecure tls;
    tls.setCACert(AMAZON_ROOT_CA1_PEM);   // simpliance.in chains to Amazon Trust Services
    tls.setTimeout(HOLIDAY_TIMEOUT_MS / 1000);

    const uint32_t deadline = millis() + HOLIDAY_TIMEOUT_MS;

    if (!tls.connect(HOLIDAY_HOST, HOLIDAY_PORT)) {
        Serial.println("[HOLIDAY] TLS connect to simpliance.in failed");
        return false;
    }

    char req[256];
    const int reqLen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: " HOLIDAY_HOST "\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0 Safari/537.36\r\n"
        "Accept: text/html\r\n"
        "Connection: close\r\n"
        "\r\n", path);
    if (reqLen <= 0 || (size_t)reqLen >= sizeof(req)) {
        Serial.println("[HOLIDAY] request did not fit");
        tls.stop();
        return false;
    }

    if (tls.print(req) == 0) {
        Serial.println("[HOLIDAY] failed to send request");
        tls.stop();
        return false;
    }

    char   line[256];
    size_t lineLen = 0;

    if (!readLine(tls, line, sizeof(line), &lineLen, deadline)) {
        Serial.println("[HOLIDAY] no response");
        tls.stop();
        return false;
    }

    const char* sp     = strchr(line, ' ');
    const int   status = (sp != nullptr) ? atoi(sp + 1) : 0;
    if (status != 200) {
        Serial.printf("[HOLIDAY] page returned HTTP %d\n", status);
        tls.stop();
        return false;
    }

    for (;;) {
        if (!readLine(tls, line, sizeof(line), &lineLen, deadline)) {
            Serial.println("[HOLIDAY] truncated headers");
            tls.stop();
            return false;
        }
        if (lineLen == 0) {
            break;
        }
    }

    // Buffered in full rather than stopped early — see the note in holiday.h
    // on why the rows this cares about can sit deep in the page. A truncated
    // read is still survivable: extractUpcoming() simply finds fewer rows.
    size_t   n         = 0;
    uint32_t idleSince = millis();

    while (n + 1 < HOLIDAY_RX_MAX && millis() < deadline) {
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

        size_t want = HOLIDAY_RX_MAX - 1 - n;
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
    return n > 0;
}

/** Resolve a state to its next few upcoming holidays. Returns false if none were found. */
static bool holidayFetch(const char* state, char* outText, size_t cap) {
    outText[0] = '\0';

    if (sRx == nullptr) {
        sRx = (char*)heap_caps_malloc(HOLIDAY_RX_MAX, MALLOC_CAP_SPIRAM);
        if (sRx == nullptr) {
            Serial.println("[HOLIDAY] receive buffer allocation failed");
            return false;
        }
    }

    char slug[HOLIDAY_STATE_MAX];
    if (!slugify(state, slug, sizeof(slug))) {
        Serial.println("[HOLIDAY] state name produced an empty slug");
        return false;
    }

    // Today's date, for filtering to what's still ahead. Declining without a
    // synced clock is honest: with no clock every holiday looks equally
    // "upcoming", which is a wrong answer dressed as a right one.
    if (!timeIsSynced()) {
        Serial.println("[HOLIDAY] clock not synced — declining lookup");
        return false;
    }
    const time_t now = time(nullptr);
    struct tm    local;
    localtime_r(&now, &local);
    const int todayY = local.tm_year + 1900;
    const int todayM = local.tm_mon + 1;
    const int todayD = local.tm_mday;

    if (!fetchYear(slug, todayY)) {
        return false;
    }
    int found = extractUpcoming(sRx, todayY, todayM, todayD,
                                outText, cap, HOLIDAY_MAX_RESULTS);

    // Year-end edge case: nothing left in the current year's table. One more
    // page, for next year, rather than telling the user "no holidays" a few
    // days before New Year's.
    if (found == 0 && fetchYear(slug, todayY + 1)) {
        found = extractUpcoming(sRx, todayY + 1, 1, 1, outText, cap, HOLIDAY_MAX_RESULTS);
    }

    return found > 0;
}

// -----------------------------------------------------------------------------
// holidayTask
// -----------------------------------------------------------------------------
static void holidayTask(void* arg) {
    (void)arg;

    for (;;) {
        HolidayReq req;
        if (xQueueReceive(sReqQ, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const char* state = req.state[0] ? req.state : HOLIDAY_DEFAULT_STATE;
        Serial.printf("[HOLIDAY] looking up \"%s\"\n", state);
        sActive.store(true);

        char text[HOLIDAYS_TEXT_MAX];
        const bool found = holidayFetch(state, text, sizeof(text));

        sActive.store(false);

        if (!found) {
            postNotice("No upcoming holidays could be found for %s.", state);
        } else {
            postNotice("Upcoming holidays in %s: %s", state, text);
        }
    }
}

bool holidayBegin() {
    if (sTask != NULL) {
        return true;
    }

    sReqQ = xQueueCreate(HOLIDAY_QUEUE_DEPTH, sizeof(HolidayReq));
    if (sReqQ == NULL) {
        Serial.println("[HOLIDAY] request queue allocation failed");
        return false;
    }

    // Core 0, with netTask and the WiFi stack: this task is networking only,
    // same placement as musicTask/newsTask.
    if (xTaskCreatePinnedToCore(holidayTask, "holiday", HOLIDAY_TASK_STACK,
                                NULL, 3, &sTask, 0) != pdPASS) {
        Serial.println("[HOLIDAY] task creation failed");
        sTask = NULL;
        return false;
    }
    return true;
}

bool holidayRequest(const char* state) {
    if (sReqQ == NULL) {
        return false;
    }

    HolidayReq req;
    snprintf(req.state, sizeof(req.state), "%s", (state != nullptr) ? state : "");

    // Supersede whatever lookup is queued or in flight.
    xQueueReset(sReqQ);

    return xQueueSend(sReqQ, &req, pdMS_TO_TICKS(50)) == pdTRUE;
}
