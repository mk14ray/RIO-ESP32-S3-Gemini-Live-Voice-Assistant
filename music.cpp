#include "music.h"
#include "jiosaavn.h"
#include "mp4aac.h"
#include "app_state.h"
#include "config.h"
#include "memmon.h"
#include "oled_display.h"
#include "secrets.h"

#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <atomic>

typedef struct {
    char query[MUSIC_QUERY_MAX];
} MusicReq;

static QueueHandle_t sReqQ  = NULL;
static TaskHandle_t  sTask  = NULL;

static uint8_t* sMoov  = nullptr;   // buffered moov box, PSRAM
static uint8_t* sFrame = nullptr;   // one AAC frame, PSRAM

static std::atomic<bool> sActive(false);
static std::atomic<bool> sPlaying(false);
static std::atomic<bool> sStop(false);

// One pending failure message for the model. Written by musicTask, drained by
// netTask; sNoticeReady is the handshake, so the buffer is only ever touched by
// one task at a time.
static char                     sNotice[160];
static std::atomic<bool>        sNoticeReady(false);

static void postNotice(const char* fmt, ...) {
    if (sNoticeReady.load()) {
        return;                 // an older, equally true failure is still queued
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sNotice, sizeof(sNotice), fmt, ap);
    va_end(ap);
    sNoticeReady.store(true);
}

bool musicIsActive() {
    return sActive.load();
}

bool musicIsPlaying() {
    return sPlaying.load();
}

void musicStop() {
    if (sActive.load()) {
        sStop.store(true);
    }
}

bool musicTakeNotice(char* dst, size_t cap) {
    if (!sNoticeReady.load() || dst == nullptr || cap == 0) {
        return false;
    }
    snprintf(dst, cap, "%s", sNotice);
    sNoticeReady.store(false);
    return true;
}

// -----------------------------------------------------------------------------
// CDN stream
// -----------------------------------------------------------------------------
/**
 * Read one CRLF-terminated line into a caller-supplied buffer.
 *
 * Fixed buffer rather than a String for the same reason as jiosaavn.cpp: String
 * reallocates on the INTERNAL heap as it grows, and doing that per header line
 * of every request — including every mid-song resume — is exactly the churn
 * that fragments the heap mbedtls needs. See memmon.h.
 */
static bool readLine(WiFiClient& c, char* out, size_t cap,
                     size_t* outLen, uint32_t deadline) {
    size_t n = 0;
    out[0] = '\0';

    for (;;) {
        if (millis() > deadline) {
            return false;
        }
        if (!c.available()) {
            if (!c.connected()) {
                return false;
            }
            delay(2);
            continue;
        }
        const int ch = c.read();
        if (ch < 0) {
            continue;
        }
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r') {
                n--;
            }
            out[n] = '\0';
            if (outLen != nullptr) *outLen = n;
            return true;
        }
        if (n + 1 < cap) {
            out[n++] = (char)ch;
            out[n]   = '\0';
        }
    }
}

/**
 * Read exactly `n` bytes, or fail.
 *
 * Everything downstream of the container header is length-addressed rather than
 * delimited — a box header is 8 bytes, a frame is however many bytes stsz says
 * — so a short read is not something to work around, it is a desync. Returning
 * false and abandoning the track is the only honest response.
 *
 * @param dst may be null, in which case the bytes are read and discarded.
 */
static bool readExact(WiFiClient& c, uint8_t* dst, size_t n, uint32_t stallMs) {
    size_t   got  = 0;
    uint32_t last = millis();

    while (got < n) {
        if (sStop.load()) {
            return false;
        }
        if (millis() - last > stallMs) {
            return false;
        }

        const int avail = c.available();
        if (avail <= 0) {
            if (!c.connected()) {
                return false;
            }
            delay(2);
            continue;
        }

        size_t want = n - got;
        if ((size_t)avail < want) {
            want = (size_t)avail;
        }

        int rd;
        if (dst != nullptr) {
            rd = c.read(dst + got, want);
        } else {
            uint8_t sink[256];
            if (want > sizeof(sink)) want = sizeof(sink);
            rd = c.read(sink, want);
        }
        if (rd <= 0) {
            delay(2);
            continue;
        }
        got  += (size_t)rd;
        last  = millis();
    }
    return true;
}

/**
 * Scale a block of PCM in place.
 *
 * Saturating rather than wrapping: MUSIC_VOLUME is below 1.0 so this cannot
 * normally clip, but a wrap would turn a loud passage into a burst of noise
 * straight into the AEC reference, and that is not a failure worth risking to
 * save a compare.
 */
static void scalePcm(int16_t* pcm, size_t samples, float gain) {
    if (gain == 1.0f) {
        return;
    }
    for (size_t i = 0; i < samples; i++) {
        float v = (float)pcm[i] * gain;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm[i] = (int16_t)v;
    }
}

/** Push mono PCM into gPcmOut, respecting the stop flag. Returns bytes sent. */
static size_t pushPcm(const int16_t* pcm, size_t samples) {
    const uint8_t* src   = (const uint8_t*)pcm;
    const size_t   bytes = samples * sizeof(int16_t);
    size_t         sent  = 0;

    // gPcmOut holds ~43 s, so this blocking is the rate limiter for the whole
    // download: TCP backpressure keeps the CDN from running minutes ahead of
    // the speaker.
    while (sent < bytes && !sStop.load()) {
        const size_t w = xStreamBufferSend(gPcmOut, src + sent, bytes - sent,
                                           pdMS_TO_TICKS(200));
        if (w == 0) {
            continue;                       // buffer full; spkTask is draining
        }
        sent += w;
    }
    return sent;
}

/** Split "http://host/path" into its two halves. */
static bool splitUrl(const char* url, char* host, size_t hostCap,
                     const char** path) {
    const char* p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char* slash = strchr(p, '/');
    if (slash == nullptr) {
        return false;
    }
    const size_t hlen = (size_t)(slash - p);
    if (hlen == 0 || hlen + 1 > hostCap) {
        return false;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';
    *path = slash;
    return true;
}

/**
 * Connect to the CDN and leave the socket positioned at the first body byte.
 *
 * `rangeFrom` resumes a transfer that died partway. MEASURED: aac.saavncdn.com
 * is Azure Blob storage and advertises Accept-Ranges: bytes, answering a ranged
 * GET with 206 — which is what makes mid-song recovery possible at all.
 */
static bool cdnConnect(WiFiClient& c, const char* host, const char* path,
                       uint64_t rangeFrom) {
    c.stop();
    c.setTimeout(MUSIC_CONNECT_TIMEOUT_MS / 1000);

    if (!c.connect(host, MUSIC_CDN_PORT)) {
        Serial.printf("[MUSIC] CDN %s unreachable\n", host);
        return false;
    }

    char req[512];
    int  n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Mozilla/5.0\r\n", path, host);
    if (rangeFrom > 0 && n > 0 && (size_t)n < sizeof(req)) {
        n += snprintf(req + n, sizeof(req) - n,
                      "Range: bytes=%lu-\r\n", (unsigned long)rangeFrom);
    }
    if (n > 0 && (size_t)n < sizeof(req)) {
        n += snprintf(req + n, sizeof(req) - n, "Connection: close\r\n\r\n");
    }
    if (n <= 0 || (size_t)n >= sizeof(req)) {
        Serial.println("[MUSIC] CDN request did not fit");
        c.stop();
        return false;
    }

    if (c.print(req) == 0) {
        c.stop();
        return false;
    }

    const uint32_t deadline = millis() + MUSIC_HEADER_TIMEOUT_MS;

    char   line[256];
    size_t lineLen = 0;
    if (!readLine(c, line, sizeof(line), &lineLen, deadline)) {
        Serial.println("[MUSIC] CDN did not respond");
        c.stop();
        return false;
    }

    const char* sp   = strchr(line, ' ');
    const int status = (sp != nullptr) ? atoi(sp + 1) : 0;

    // 200 for a fresh fetch, 206 for a resume. A 200 in reply to a Range request
    // means the server ignored it and restarted from zero, which would splice
    // the beginning of the song into the middle — refuse rather than play that.
    const int want = (rangeFrom > 0) ? 206 : 200;
    if (status != want) {
        Serial.printf("[MUSIC] CDN returned HTTP %d (wanted %d)\n", status, want);
        c.stop();
        return false;
    }

    for (;;) {
        if (!readLine(c, line, sizeof(line), &lineLen, deadline)) {
            c.stop();
            return false;
        }
        if (lineLen == 0) {
            return true;                    // blank line: body starts here
        }
    }
}

/** Stream and play one track. Returns false if nothing was ever played. */
static bool streamFromCdn(const char* url, const char* title) {
    char        host[128];
    const char* path = nullptr;
    if (!splitUrl(url, host, sizeof(host), &path)) {
        postNotice("The song could not be played: the audio address was unusable.");
        return false;
    }

    WiFiClient c;
    if (!cdnConnect(c, host, path, 0)) {
        postNotice("The music service could not be reached, so the song cannot play.");
        return false;
    }

    // --- Walk the container to the sample tables -----------------------------
    // filePos tracks the absolute offset from the start of the file, because a
    // resume has to ask for a byte number and the frame loop only knows frames.
    Mp4AacInfo info;
    bool     opened  = false;
    uint64_t filePos = 0;

    for (;;) {
        uint8_t hdr[8];
        if (!readExact(c, hdr, sizeof(hdr), MUSIC_STALL_TIMEOUT_MS)) {
            c.stop();
            if (!sStop.load()) {
                Serial.println("[MUSIC] stream ended inside the container header");
                postNotice("The song could not be played: the audio file was incomplete.");
            }
            return false;
        }
        filePos += sizeof(hdr);

        const uint32_t boxSize = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                               | ((uint32_t)hdr[2] << 8)  |  (uint32_t)hdr[3];
        const char* type = (const char*)(hdr + 4);

        if (boxSize < 8) {
            c.stop();
            Serial.printf("[MUSIC] bogus box size %u\n", (unsigned)boxSize);
            postNotice("The song could not be played: the audio file was malformed.");
            return false;
        }

        if (memcmp(type, "moov", 4) == 0) {
            if (boxSize > MUSIC_MOOV_MAX) {
                c.stop();
                Serial.printf("[MUSIC] moov is %u bytes, over the %u cap\n",
                              (unsigned)boxSize, (unsigned)MUSIC_MOOV_MAX);
                postNotice("The song is too long for this device to index, so it did not play.");
                return false;
            }
            memcpy(sMoov, hdr, sizeof(hdr));
            if (!readExact(c, sMoov + 8, boxSize - 8, MUSIC_STALL_TIMEOUT_MS)) {
                c.stop();
                if (!sStop.load()) {
                    postNotice("The song could not be played: the audio index was truncated.");
                }
                return false;
            }
            filePos += boxSize - 8;
            if (!mp4aacOpen(sMoov, boxSize, &info)) {
                c.stop();
                postNotice("The song could not be played: its audio format is unsupported.");
                return false;
            }
            opened = true;
            continue;
        }

        if (memcmp(type, "mdat", 4) == 0) {
            if (!opened) {
                // moov after mdat would mean seeking backwards on a socket that
                // cannot. MEASURED: JioSaavn never does this, every file is
                // faststart. Bail loudly rather than silently play nothing.
                c.stop();
                Serial.println("[MUSIC] mdat before moov — file is not faststart");
                postNotice("The song could not be played: the audio file is not streamable.");
                return false;
            }
            break;                          // positioned at the first AAC frame
        }

        // ftyp, free, udta, anything else: step over it.
        if (!readExact(c, nullptr, boxSize - 8, MUSIC_STALL_TIMEOUT_MS)) {
            c.stop();
            return false;
        }
        filePos += boxSize - 8;
    }

    // --- Decode and play -----------------------------------------------------
    Serial.printf("[MUSIC] playing \"%s\"  (%s)\n", title, url);
    oledTriggerTool(OLED_TOOL_PLAY);   // one-shot: playback has begun
    oledMusicStart(title);             // persistent: the now-playing strip

    sActive.store(true);

    bool          playedSomething = false;
    unsigned long pcmBytes        = 0;
    uint32_t      badRun          = 0;      // CONSECUTIVE decode failures
    uint32_t      badTotal        = 0;      // for the summary only
    uint32_t      resumes         = 0;
    const char*   why             = "reached the end of the track";
    uint32_t      lastFrame       = 0;

    for (uint32_t i = 0; i < info.frameCount && !sStop.load(); i++) {
        const uint32_t fsz = mp4aacFrameBytes(i);
        if (fsz == 0 || fsz > MUSIC_MAX_FRAME_BYTES) {
            Serial.printf("[MUSIC] frame %u has size %u — stopping\n",
                          (unsigned)i, (unsigned)fsz);
            why = "the frame table went out of range";
            break;
        }

        if (!readExact(c, sFrame, fsz, MUSIC_STALL_TIMEOUT_MS)) {
            if (sStop.load()) {
                why = "it was interrupted";
                break;
            }

            // A four-minute song is four minutes of WiFi, and a dropped socket
            // partway through used to end the track. Reconnect with a Range
            // header at this frame's byte offset and carry on: filePos has not
            // advanced, so whatever the failed read half-consumed is refetched.
            // AAC-LC frames decode independently, so the seam is inaudible.
            if (++resumes > MUSIC_MAX_RESUMES) {
                Serial.printf("[MUSIC] gave up after %u resume attempts at frame %u\n",
                              (unsigned)resumes, (unsigned)i);
                why = "the connection kept dropping";
                break;
            }

            Serial.printf("[MUSIC] stream broke at frame %u/%u — resuming from byte %lu "
                          "(attempt %u)\n",
                          (unsigned)i, (unsigned)info.frameCount,
                          (unsigned long)filePos, (unsigned)resumes);

            if (!cdnConnect(c, host, path, filePos)) {
                why = "the connection could not be re-established";
                break;
            }
            i--;                            // refetch this frame
            continue;
        }
        filePos += fsz;

        const int16_t* pcm = nullptr;
        const int n = mp4aacDecodeFrame(sFrame, fsz, &pcm);
        if (n <= 0) {
            // CONSECUTIVE, not cumulative. Counting every failure across a whole
            // track would abandon a 15,000-frame song over 64 scattered glitches
            // spread across six minutes — which is exactly the "handful of bad
            // frames" this is supposed to tolerate. Only an unbroken wall of
            // them means the stream is not what the decoder was set up for.
            badTotal++;
            if (++badRun > MUSIC_MAX_DECODE_ERRORS) {
                Serial.printf("[MUSIC] %u consecutive decode errors at frame %u "
                              "— stopping\n", (unsigned)badRun, (unsigned)i);
                why = "the audio stream could not be decoded";
                break;
            }
            continue;
        }
        badRun = 0;

        scalePcm((int16_t*)pcm, (size_t)n, MUSIC_VOLUME);
        const size_t sent = pushPcm(pcm, (size_t)n);
        if (sent > 0) {
            playedSomething = true;
            sPlaying.store(true);
            pcmBytes += (unsigned long)sent;
        }
        lastFrame = i;

        // Yield, every single frame, unconditionally.
        //
        // This is not a politeness delay — without it the device reboots. Every
        // call above returns immediately while the fast path is available:
        // readExact() when the socket already holds the bytes, the decoder
        // always, and xStreamBufferSend() while gPcmOut has room. gPcmOut holds
        // ~43 s, so for the first 43 s of a song NOTHING in this loop blocks,
        // musicTask (priority 3) spins flat out on core 0, IDLE0 (priority 0)
        // never gets scheduled, and the task watchdog panics the chip.
        // MEASURED: it reached 23 s buffered and then aborted with
        // "task_wdt: IDLE0 (CPU 0) / CPU 0: music".
        //
        // One tick costs ~1 ms against a frame that is ~23 ms of audio, so this
        // still fills the buffer many times faster than the speaker drains it.
        // Once the buffer is full, pushPcm() blocks and paces us naturally.
        vTaskDelay(1);

        // Once every ~10 s of audio. Without this a stop partway through is
        // indistinguishable from a stop at the start in the log.
        if ((i % 512) == 0) {
            Serial.printf("[MUSIC] frame %u/%u  %lu s buffered  heap=%u\n",
                          (unsigned)i, (unsigned)info.frameCount,
                          pcmBytes / (2UL * SPK_SAMPLE_RATE),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }

    c.stop();
    mp4aacClose();
    sPlaying.store(false);
    sActive.store(false);
    oledMusicStop();               // covers every exit: finished, stopped, failed

    Serial.printf("[MUSIC] stopped at frame %u/%u after %lu s of audio "
                  "(%u decode errors, %u resumes): %s\n",
                  (unsigned)lastFrame, (unsigned)info.frameCount,
                  pcmBytes / (2UL * SPK_SAMPLE_RATE),
                  (unsigned)badTotal, (unsigned)resumes,
                  sStop.load() ? "it was interrupted" : why);

    if (!playedSomething) {
        postNotice("The song was found but no audio came back, so it did not play.");
        return false;
    }
    return true;
}


// -----------------------------------------------------------------------------
// musicTask
// -----------------------------------------------------------------------------
static void musicTask(void* arg) {
    (void)arg;

    for (;;) {
        MusicReq req;
        if (xQueueReceive(sReqQ, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Clear a stop that arrived while this request was still queued — it
        // belonged to the previous song, not to this one.
        sStop.store(false);

        Serial.printf("[MUSIC] looking up \"%s\"\n", req.query);
        sActive.store(true);

        // Checked here as well as inside jiosaavnResolve() so the user gets an
        // accurate reason. Without this the refusal surfaces as "no song was
        // found", which is not what happened and would send them hunting for a
        // different title over a memory problem.
        if (!memHaveInternal(MUSIC_LOOKUP_INTERNAL_NEED, "song lookup")) {
            memLog("lookup declined");
            sActive.store(false);
            postNotice("There is not enough free memory to start a song right now. "
                       "Ask again in a moment.");
            continue;
        }

        char     url[JIOSAAVN_URL_MAX];
        char     title[MUSIC_TITLE_MAX];
        uint32_t secs = 0;

        const bool found = jiosaavnResolve(req.query, url, sizeof(url),
                                           title, sizeof(title), &secs);

        // A stop that lands during the lookup still counts: the user changed
        // their mind while the request was in flight.
        if (sStop.load()) {
            sPlaying.store(false);
            sActive.store(false);
            Serial.println("[MUSIC] cancelled during lookup");
            continue;
        }

        if (!found) {
            sActive.store(false);
            postNotice("No song was found for %s, so nothing is playing.", req.query);
            continue;
        }

        Serial.printf("[MUSIC] resolved to \"%s\" (%u s)\n",
                      title, (unsigned)secs);

        memLog("before stream");
        streamFromCdn(url, title);
        sPlaying.store(false);
        sActive.store(false);
        memLog("after stream");
    }
}

bool musicBegin() {
    if (sTask != NULL) {
        return true;
    }

    sReqQ = xQueueCreate(MUSIC_QUEUE_DEPTH, sizeof(MusicReq));
    if (sReqQ == NULL) {
        Serial.println("[MUSIC] request queue allocation failed");
        return false;
    }

    sMoov = (uint8_t*)heap_caps_malloc(MUSIC_MOOV_MAX, MALLOC_CAP_SPIRAM);
    if (sMoov == nullptr) {
        Serial.println("[MUSIC] moov buffer allocation failed");
        return false;
    }

    // PSRAM, despite the decoder reading across it every frame. Internal RAM is
    // the scarce resource here: MEASURED on hardware, the JioSaavn TLS handshake
    // takes internal free heap down to a few hundred bytes while the Gemini
    // session is up, so 8 KB back matters more than the access latency on a
    // buffer that is read strictly sequentially.
    sFrame = (uint8_t*)heap_caps_malloc(MUSIC_MAX_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (sFrame == nullptr) {
        Serial.println("[MUSIC] frame buffer allocation failed");
        return false;
    }

    if (!mp4aacBegin()) {
        return false;
    }

    // Core 0, with netTask and the WiFi stack: this task is networking plus a
    // decoder, and has no business adding jitter to the audio core.
    if (xTaskCreatePinnedToCore(musicTask, "music", MUSIC_TASK_STACK,
                                NULL, 3, &sTask, 0) != pdPASS) {
        Serial.println("[MUSIC] task creation failed");
        sTask = NULL;
        return false;
    }
    return true;
}

bool musicRequest(const char* songName) {
    if (sReqQ == NULL || songName == nullptr || *songName == '\0') {
        return false;
    }

    MusicReq req;
    snprintf(req.query, sizeof(req.query), "%s", songName);

    // Supersede whatever is playing or waiting. Without the stop, a new request
    // would sit in the queue until the current song ran out — minutes after the
    // user asked for a different one.
    musicStop();
    xQueueReset(sReqQ);
    gFlushPlayback.store(true);

    return xQueueSend(sReqQ, &req, pdMS_TO_TICKS(50)) == pdTRUE;
}
