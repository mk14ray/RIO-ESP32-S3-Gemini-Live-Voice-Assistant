#include "news.h"
#include "config.h"
#include "secrets.h"
#include "memmon.h"

#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <ctype.h>
#include <stdarg.h>

typedef struct {
    char query[NEWS_QUERY_MAX];
} NewsReq;

static QueueHandle_t sReqQ = NULL;
static TaskHandle_t  sTask = NULL;

// Receive staging. PSRAM and file-static rather than stack, same reasoning as
// jiosaavn.cpp's sRx: newsTask is the only caller and NEWS_RX_MAX does not
// belong on a stack.
static char* sRx = nullptr;

static std::atomic<bool> sActive(false);

// One pending message for the model, success or failure. Written by newsTask,
// drained by netTask; sNoticeReady is the handshake, so the buffer is only
// ever touched by one task at a time. Sized for NEWS_HEADLINES_MAX plus the
// sentence wrapped around it.
static char               sNotice[NEWS_HEADLINES_MAX + 96];
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

bool newsIsActive() {
    return sActive.load();
}

bool newsTakeNotice(char* dst, size_t cap) {
    if (!sNoticeReady.load() || dst == nullptr || cap == 0) {
        return false;
    }
    snprintf(dst, cap, "%s", sNotice);
    sNoticeReady.store(false);
    return true;
}

// -----------------------------------------------------------------------------
// URL-encode a query for the RSS search path. Same rules as
// jiosaavn.cpp's jiosaavnUrlEncode, kept as its own copy so this file has no
// dependency on jiosaavn.cpp for one small helper.
// -----------------------------------------------------------------------------
static bool urlEncode(const char* src, char* dst, size_t cap) {
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
            if (o + 3 >= cap) return false;
            static const char HEXD[] = "0123456789ABCDEF";
            dst[o++] = '%';
            dst[o++] = HEXD[c >> 4];
            dst[o++] = HEXD[c & 0x0F];
        }
    }
    dst[o] = '\0';
    return true;
}

/**
 * Read one CRLF-terminated line into a caller-supplied buffer.
 *
 * Fixed buffer rather than a String for the same reason as jiosaavn.cpp:
 * String reallocates on the INTERNAL heap as it grows, and doing that per
 * header line is exactly the churn that fragments the heap mbedtls needs.
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
 * Decode the handful of XML entities Google actually emits inside <title>:
 * &amp; &lt; &gt; &quot; &#39; &apos;. In place, since decoding never grows
 * the string.
 */
static void decodeXmlEntities(char* s) {
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

/**
 * Pull up to `maxHeadlines` <title> values out of the RSS body and join them
 * with "; " into `out`.
 *
 * strstr is safe here (unlike gemini_live.cpp's WebSocket scan) because sRx is
 * always NUL-terminated after the read loop below. The scan starts at the
 * first <item> so the channel-level <title> ("<topic> - Google News") is
 * never mistaken for a headline.
 *
 * @return the number of headlines found.
 */
static int extractHeadlines(const char* xml, char* out, size_t outCap) {
    int    count   = 0;
    size_t written = 0;
    out[0] = '\0';

    const char* p = strstr(xml, "<item>");
    while (p != nullptr && count < NEWS_MAX_HEADLINES) {
        const char* itemEnd = strstr(p, "</item>");
        const char* ts      = strstr(p, "<title>");
        if (ts == nullptr || (itemEnd != nullptr && ts > itemEnd)) {
            break;   // no more items carry a title
        }
        ts += 7;   // strlen("<title>")
        const char* te = strstr(ts, "</title>");
        if (te == nullptr || (itemEnd != nullptr && te > itemEnd)) {
            break;
        }

        char title[NEWS_TITLE_MAX];
        size_t tlen = (size_t)(te - ts);
        if (tlen >= sizeof(title)) {
            tlen = sizeof(title) - 1;
        }
        memcpy(title, ts, tlen);
        title[tlen] = '\0';
        decodeXmlEntities(title);

        const int n = snprintf(out + written, outCap - written, "%s%s",
                               (count > 0) ? "; " : "", title);
        if (n < 0 || (size_t)n >= outCap - written) {
            break;   // no more room; keep what was already collected
        }
        written += (size_t)n;
        count++;

        p = strstr(itemEnd != nullptr ? itemEnd : te, "<item>");
    }
    return count;
}

/** Resolve a topic to a joined headline string. Returns false if none were found. */
static bool newsFetch(const char* topic, char* outHeadlines, size_t cap) {
    outHeadlines[0] = '\0';

    if (sRx == nullptr) {
        sRx = (char*)heap_caps_malloc(NEWS_RX_MAX, MALLOC_CAP_SPIRAM);
        if (sRx == nullptr) {
            Serial.println("[NEWS] receive buffer allocation failed");
            return false;
        }
    }

    char enc[NEWS_QUERY_MAX * 3 + 1];
    if (!urlEncode(topic, enc, sizeof(enc))) {
        Serial.println("[NEWS] topic too long to encode");
        return false;
    }

    // Checked before opening a second TLS session against the live Gemini
    // socket, same reasoning as MUSIC_LOOKUP_INTERNAL_NEED — declining a
    // lookup is a spoken "not right now", running out of heap mid-handshake
    // is a reboot that loses the whole conversation.
    if (!memHaveInternal(NEWS_LOOKUP_INTERNAL_NEED, "news lookup")) {
        return false;
    }

    WiFiClientSecure tls;
    tls.setCACert(GTS_ROOT_R1_PEM);   // news.google.com chains to Google Trust Services
    tls.setTimeout(NEWS_TIMEOUT_MS / 1000);

    const uint32_t deadline = millis() + NEWS_TIMEOUT_MS;

    if (!tls.connect(NEWS_HOST, NEWS_PORT)) {
        Serial.println("[NEWS] TLS connect to news.google.com failed");
        return false;
    }

    // No Accept-Encoding header, same reasoning as jiosaavn.cpp: nothing here
    // can inflate gzip, and omitting it is what keeps the response plain.
    char req[512];
    const int reqLen = snprintf(req, sizeof(req),
        "GET " NEWS_RSS_PATH "%s" NEWS_RSS_SUFFIX " HTTP/1.1\r\n"
        "Host: " NEWS_HOST "\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0 Safari/537.36\r\n"
        "Accept: application/rss+xml\r\n"
        "Connection: close\r\n"
        "\r\n", enc);
    if (reqLen <= 0 || (size_t)reqLen >= sizeof(req)) {
        Serial.println("[NEWS] request did not fit");
        tls.stop();
        return false;
    }

    if (tls.print(req) == 0) {
        Serial.println("[NEWS] failed to send search request");
        tls.stop();
        return false;
    }

    // --- Status line + headers -------------------------------------------
    char   line[256];
    size_t lineLen = 0;

    if (!readLine(tls, line, sizeof(line), &lineLen, deadline)) {
        Serial.println("[NEWS] no response to search request");
        tls.stop();
        return false;
    }

    const char* sp     = strchr(line, ' ');
    const int   status = (sp != nullptr) ? atoi(sp + 1) : 0;
    if (status != 200) {
        Serial.printf("[NEWS] search returned HTTP %d\n", status);
        tls.stop();
        return false;
    }

    for (;;) {
        if (!readLine(tls, line, sizeof(line), &lineLen, deadline)) {
            Serial.println("[NEWS] truncated headers");
            tls.stop();
            return false;
        }
        if (lineLen == 0) {
            break;
        }
    }

    // --- Body --------------------------------------------------------------
    // Read until the socket closes, the buffer fills, or enough items have
    // arrived to satisfy NEWS_MAX_HEADLINES — whichever comes first. The feed
    // lists results most-relevant-first, so stopping early never costs the
    // headlines this cares about, only ones nobody was going to hear anyway.
    size_t   n         = 0;
    uint32_t idleSince = millis();
    int      itemsSeen = 0;

    while (n + 1 < NEWS_RX_MAX && millis() < deadline) {
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

        size_t want = NEWS_RX_MAX - 1 - n;
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

        sRx[n] = '\0';
        itemsSeen = 0;
        const char* scan = sRx;
        while ((scan = strstr(scan, "</item>")) != nullptr) {
            itemsSeen++;
            scan += 7;
        }
        if (itemsSeen >= NEWS_MAX_HEADLINES) {
            break;
        }
    }

    tls.stop();
    sRx[n] = '\0';

    if (n == 0) {
        Serial.println("[NEWS] empty search response");
        return false;
    }

    const int found = extractHeadlines(sRx, outHeadlines, cap);
    if (found == 0) {
        Serial.printf("[NEWS] no headlines for \"%s\" (%u bytes scanned)\n",
                      topic, (unsigned)n);
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// newsTask
// -----------------------------------------------------------------------------
static void newsTask(void* arg) {
    (void)arg;

    for (;;) {
        NewsReq req;
        if (xQueueReceive(sReqQ, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        Serial.printf("[NEWS] looking up \"%s\"\n", req.query);
        sActive.store(true);

        char headlines[NEWS_HEADLINES_MAX];
        const bool found = newsFetch(req.query, headlines, sizeof(headlines));

        sActive.store(false);

        if (!found) {
            postNotice("No recent news was found for %s.", req.query);
        } else {
            postNotice("Recent headlines about %s: %s", req.query, headlines);
        }
    }
}

bool newsBegin() {
    if (sTask != NULL) {
        return true;
    }

    sReqQ = xQueueCreate(NEWS_QUEUE_DEPTH, sizeof(NewsReq));
    if (sReqQ == NULL) {
        Serial.println("[NEWS] request queue allocation failed");
        return false;
    }

    // Core 0, with netTask and the WiFi stack: this task is networking only,
    // same placement as musicTask and for the same reason.
    if (xTaskCreatePinnedToCore(newsTask, "news", NEWS_TASK_STACK,
                                NULL, 3, &sTask, 0) != pdPASS) {
        Serial.println("[NEWS] task creation failed");
        sTask = NULL;
        return false;
    }
    return true;
}

bool newsRequest(const char* topic) {
    if (sReqQ == NULL || topic == nullptr || *topic == '\0') {
        return false;
    }

    NewsReq req;
    snprintf(req.query, sizeof(req.query), "%s", topic);

    // Supersede whatever lookup is queued or in flight. A second "what about
    // X" should not wait behind an earlier topic nobody cares about anymore.
    xQueueReset(sReqQ);

    return xQueueSend(sReqQ, &req, pdMS_TO_TICKS(50)) == pdTRUE;
}
