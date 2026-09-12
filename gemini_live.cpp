#include "gemini_live.h"
#include "ws_client.h"
#include "app_state.h"
#include "b64.h"
#include "camera.h"
#include "music.h"
#include "news.h"
#include "holiday.h"
#include "wifi_mgr.h"
#include "config.h"
#include "secrets.h"
#include "oled_display.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <ctype.h>

#define STR_HELPER(x) #x
#define TOSTR(x)      STR_HELPER(x)

static WsClient  ws;
static char*     scratch      = nullptr;   // setup-message staging (PSRAM)
static const size_t SCRATCH_BYTES = 8192;

static volatile bool sSetupComplete = false;
static volatile bool sGoAway        = false;
static String        sResumeHandle;

// -----------------------------------------------------------------------------
// Camera permission gate
//
// The model cannot open this gate on its own. Its first call for a request is
// always refused, which forces it to ask out loud; only a *subsequent* call that
// arrives after the user has actually taken a turn of their own can be granted.
// sUserTurnSeq is what makes that check real: it counts turns the user spoke,
// observed from inputTranscription, so "the model asked and something came back"
// cannot be faked by the model calling twice in a row.
//
// What this deliberately does NOT do is judge whether the user said yes or no —
// that is the model's job, per the system instruction. The gate guarantees a
// question was asked and answered, not the content of the answer.
//
// All of this is touched only by netTask (parse callback and service run there).
// -----------------------------------------------------------------------------
enum CamGate : uint8_t { GATE_IDLE = 0, GATE_AWAITING_ANSWER };

static uint8_t  sGate             = GATE_IDLE;
static uint32_t sGateArmedAt      = 0;
static uint32_t sGateArmedModelSeq = 0;

static uint32_t sUserTurnSeq  = 0;
static bool     sUserTurnOpen = false;

// Model turns finished, and the user-turn count as it stood at the end of the
// most recent one. Together these pin the answer to the question: a user turn
// only counts as consent if it started after the model stopped talking, so a
// transcription fragment that lands late — after the tool call but before the
// model has actually asked anything — cannot be mistaken for a reply.
static uint32_t sModelTurnSeq      = 0;
static uint32_t sUserSeqAtTurnEnd  = 0;

static bool sHasPendingCall = false;
static char sPendingCallId[80];
static char sPendingCallName[64];

// Free-text tool argument (song name, news topic, or holiday state — whichever
// tool was called). Unlike the id and the name this is NOT run through
// isSafeJsonToken: it is free text the user spoke, it may be Devanagari or
// contain apostrophes, and rejecting it on those grounds would refuse
// perfectly ordinary requests. It is escaped at the point it is written back
// into JSON instead — see jsonEscape.
static char sPendingCallQuery[MUSIC_QUERY_MAX];

/**
 * Accept only characters that cannot break out of a JSON string.
 *
 * The tool response is hand-built with snprintf, so an id echoed back from the
 * wire is untrusted input to our own JSON writer. Everything Google sends is
 * plain identifier text; anything else is refused rather than escaped.
 */
static bool sendToolResponse(const char* id, const char* name, const char* result);

/**
 * Escape `src` into a JSON string body (no surrounding quotes).
 *
 * Needed because song titles are free text on their way back out to the model,
 * and a stray quote or backslash in one would corrupt the hand-built message
 * exactly the way a stray quote in SYSTEM_INSTRUCTION would. Control characters
 * are dropped rather than \u-escaped: none belong in a title, and dropping them
 * cannot produce a malformed document.
 *
 * @return false if the escaped result would not fit.
 */
static bool jsonEscape(const char* src, char* dst, size_t cap) {
    if (src == nullptr || dst == nullptr || cap == 0) {
        return false;
    }

    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p; p++) {
        const unsigned char c = *p;

        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) return false;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            continue;                    // control character: drop it
        } else {
            if (o + 1 >= cap) return false;
            dst[o++] = (char)c;          // UTF-8 continuation bytes pass through
        }
    }

    dst[o] = '\0';
    return true;
}

static bool isSafeJsonToken(const char* s) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    for (const char* p = s; *p; p++) {
        const bool ok = isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// PSRAM allocator for ArduinoJson — keeps control-message parsing off the
// internal heap, which is contended by mbedTLS.
// -----------------------------------------------------------------------------
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override {
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    }
    void deallocate(void* p) override {
        heap_caps_free(p);
    }
    void* reallocate(void* p, size_t n) override {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    }
};
static PsramAllocator sJsonAlloc;

// -----------------------------------------------------------------------------
// Byte-exact substring search. The reassembly buffer is not NUL-terminated, so
// strstr() is not safe here.
// -----------------------------------------------------------------------------
static int findToken(const uint8_t* buf, size_t len, size_t from, const char* needle) {
    const size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen || from > len - nlen) {
        return -1;
    }
    for (size_t i = from; i + nlen <= len; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// -----------------------------------------------------------------------------
// Audio extraction: raw scan, no JSON DOM.
//
// This is safe precisely because base64 output can never contain '"' or '\',
// so the closing quote of a "data" value is unambiguous with zero escape
// tracking. (Transcription text CAN contain \" — which is exactly why that goes
// through a real parser below and this does not.)
// -----------------------------------------------------------------------------
static void extractAudio(const uint8_t* buf, size_t len) {
    size_t pos = 0;
    uint8_t out[B64_DECODE_OUT];

    while (pos < len) {
        int anchor = findToken(buf, len, pos, "\"inlineData\"");
        if (anchor < 0) {
            break;
        }

        int d = findToken(buf, len, (size_t)anchor + 12, "\"data\"");
        if (d < 0) {
            break;
        }

        // Skip whitespace, require ':', skip whitespace, require '"'.
        // Matching a fixed "data":" literal would silently drop ALL audio if
        // Google ever emitted a space after the colon.
        size_t i = (size_t)d + 6;
        while (i < len && isspace((int)buf[i])) i++;
        if (i >= len || buf[i] != ':') { pos = (size_t)d + 6; continue; }
        i++;
        while (i < len && isspace((int)buf[i])) i++;
        if (i >= len || buf[i] != '"') { pos = (size_t)d + 6; continue; }
        i++;

        const size_t spanStart = i;
        while (i < len && buf[i] != '"') i++;
        if (i >= len) {
            break;   // truncated message
        }
        const size_t spanEnd = i;
        const size_t spanLen = spanEnd - spanStart;

        // First audio of a turn: take ownership of the LISTENING -> SPEAKING edge.
        if (spanLen > 0) {
            uint8_t expect = CONV_LISTENING;
            if (gConv.compare_exchange_strong(expect, CONV_SPEAKING)) {
                // RIO is about to speak, and music (if any) is being written
                // into the very same buffer. Stop it here rather than letting
                // the two interleave into one garbled stream — this is what
                // makes "stop the music" work without a tool call at all, since
                // any reply at all silences the song.
                //
                // Order matters: musicTask checks the stop flag every
                // MUSIC_RX_CHUNK bytes (~43 ms), so stopping it before the flush
                // below means almost nothing of the song survives the reset.
                // musicIsPlaying(), NOT musicIsActive(): the model answers
                // "playing it now" the instant play_song is serviced, seconds
                // before the lookup finishes. Testing musicIsActive() here made
                // that confirmation cancel the song it was confirming, so no
                // song could ever start. Once audio is actually flowing, any
                // reply still silences it, which is the behaviour intended here.
                if (musicIsPlaying()) {
                    Serial.println("[MUSIC] stopping — RIO is answering");
                    musicStop();
                    gFlushPlayback.store(true);
                }

                // Clear any stale turnComplete before the turn starts. Without
                // this, a previous turn that produced no audio would leave the
                // flag set and spkTask would cut this reply off the first time
                // the buffer momentarily drained.
                gTurnComplete.store(false);
                Serial.println("[STATE] speaking");
                oledSetMode(OLED_MODE_SPEAKING);
                // New utterance: drop whatever caption is still on screen so a
                // reply never appears to continue the previous turn's sentence.
                oledSubtitleClear();
            }
        }

        // Decode in 4-char-aligned chunks straight into the playback buffer.
        size_t off = 0;
        while (off < spanLen) {
            size_t take = spanLen - off;
            if (take > B64_DECODE_CHUNK) {
                take = B64_DECODE_CHUNK;
            }

            size_t decoded = 0;
            if (!b64Decode((const char*)(buf + spanStart + off), take,
                           out, sizeof(out), &decoded)) {
                break;
            }

            if (decoded > 0) {
                size_t sent = xStreamBufferSend(gPcmOut, out, decoded, pdMS_TO_TICKS(2000));
                if (sent < decoded) {
                    gDroppedChunks.fetch_add(1);
                }
            }
            off += take;
        }

        pos = spanEnd + 1;
    }
}

// -----------------------------------------------------------------------------
// Control-field parsing: ArduinoJson with a filter.
//
// `serverContent.modelTurn` is deliberately absent from the filter, so
// ArduinoJson walks past the base64 without ever allocating it.
// -----------------------------------------------------------------------------
static void parseControl(const uint8_t* buf, size_t len) {
    JsonDocument filter(&sJsonAlloc);
    filter["setupComplete"]           = true;
    filter["goAway"]                  = true;
    filter["sessionResumptionUpdate"] = true;
    filter["toolCall"]                = true;
    JsonObject sc = filter["serverContent"].to<JsonObject>();
    sc["turnComplete"]        = true;
    sc["interrupted"]         = true;
    sc["generationComplete"]  = true;
    sc["inputTranscription"]  = true;
    sc["outputTranscription"] = true;

    JsonDocument doc(&sJsonAlloc);
    DeserializationError err = deserializeJson(doc, (const char*)buf, len,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[GEMINI] control parse failed: %s\n", err.c_str());
        return;
    }

    if (!doc["setupComplete"].isNull()) {
        sSetupComplete = true;
        Serial.println("[GEMINI] setupComplete");
    }

    if (!doc["goAway"].isNull()) {
        sGoAway = true;
        Serial.println("[GEMINI] goAway — will recycle at the next turn boundary");
    }

    JsonVariant sru = doc["sessionResumptionUpdate"];
    if (!sru.isNull() && sru["resumable"].as<bool>()) {
        const char* h = sru["newHandle"];
        if (h != nullptr) {
            sResumeHandle = h;
        }
    }

    JsonVariant tc = doc["toolCall"];
    if (!tc.isNull()) {
        JsonArray calls = tc["functionCalls"];
        for (JsonObject call : calls) {
            const char* id   = call["id"];
            const char* name = call["name"];

            if (sHasPendingCall) {
                // Every call MUST be answered. The model blocks until it has a
                // response for each id it issued, so dropping one wedges the
                // session permanently: audio keeps flowing and transcripts keep
                // arriving, but no reply is ever generated again. Refuse it
                // immediately rather than leaving it unanswered.
                Serial.printf("[GEMINI] second tool call while one is pending: %s\n",
                              name ? name : "?");
                if (isSafeJsonToken(id) && isSafeJsonToken(name)) {
                    sendToolResponse(id, name, "busy");
                }
                continue;
            }
            if (!isSafeJsonToken(id) || !isSafeJsonToken(name)) {
                Serial.println("[GEMINI] tool call with unusable id/name — ignored");
                continue;
            }

            snprintf(sPendingCallId, sizeof(sPendingCallId), "%s", id);
            snprintf(sPendingCallName, sizeof(sPendingCallName), "%s", name);

            // `args` is already in the document: filter["toolCall"] = true keeps
            // the whole subtree, so no filter change was needed for this.
            sPendingCallQuery[0] = '\0';
            const char* q = call["args"]["query"];
            if (q == nullptr) {
                q = call["args"]["state"];   // get_holidays names its argument differently
            }
            if (q != nullptr) {
                snprintf(sPendingCallQuery, sizeof(sPendingCallQuery), "%s", q);
            }

            sHasPendingCall = true;
            Serial.printf("[GEMINI] <-- toolCall %s%s%s\n", sPendingCallName,
                          sPendingCallQuery[0] ? " query=" : "", sPendingCallQuery);
        }
    }

    JsonVariant content = doc["serverContent"];
    if (content.isNull()) {
        return;
    }

    const char* inTxt = content["inputTranscription"]["text"];
    if (inTxt != nullptr && strlen(inTxt) > 0) {
        // Count the START of each user turn, not each fragment: transcription
        // arrives in pieces, and the gate must not read one long sentence as
        // several separate answers.
        if (!sUserTurnOpen) {
            sUserTurnOpen = true;
            sUserTurnSeq++;
        }
#if LOG_TRANSCRIPTS
        Serial.printf("[YOU] %s\n", inTxt);
#endif
        // Outside the LOG_TRANSCRIPTS guard on purpose: the captions are a
        // product feature, not debug output, and must survive logging being off.
        oledSubtitleUser(inTxt);
    }

    const char* outTxt = content["outputTranscription"]["text"];
    if (outTxt != nullptr && strlen(outTxt) > 0) {
#if LOG_TRANSCRIPTS
        Serial.printf("[RIO] %s\n", outTxt);
#endif
        oledSubtitleRio(outTxt);
    }

    if (content["interrupted"].as<bool>()) {
        // spkTask performs the actual reset — resetting a stream buffer while
        // another task is blocked on it is undefined.
        gFlushPlayback.store(true);
        Serial.println("[GEMINI] interrupted");
    }

    if (content["turnComplete"].as<bool>()) {
        // The model has stopped talking, so whatever the user says next is a
        // new turn — including the answer to a permission question.
        sUserTurnOpen     = false;
        sModelTurnSeq++;
        sUserSeqAtTurnEnd = sUserTurnSeq;

        // Only meaningful while a reply is actually playing. If the turn
        // produced no audio at all we stay in LISTENING and must NOT latch the
        // flag, or it would terminate the following turn early.
        if (gConv.load() == CONV_SPEAKING) {
            gTurnComplete.store(true);
        }
    }
}

static void onWsEvent(WsEvent ev, const uint8_t* payload, size_t len, void* user) {
    (void)user;
    switch (ev) {
        case WS_MESSAGE:
            extractAudio(payload, len);
            parseControl(payload, len);
            break;
        case WS_OVERFLOW:
            gOverflows.fetch_add(1);
            break;
        case WS_DISCONNECTED:
            sSetupComplete = false;
            // A permission granted in the old session does not carry into the
            // new one, and a call id from a dead session is meaningless.
            sHasPendingCall = false;
            sGate           = GATE_IDLE;
            sUserTurnOpen   = false;
            break;
        case WS_CONNECTED:
            // Counters are compared against snapshots, so they must start from
            // a known point rather than carry across a resumed session.
            sUserTurnSeq      = 0;
            sModelTurnSeq     = 0;
            sUserSeqAtTurnEnd = 0;
            break;
        default:
            break;
    }
}

bool geminiBegin() {
    if (scratch == nullptr) {
        scratch = (char*)heap_caps_malloc(SCRATCH_BYTES, MALLOC_CAP_SPIRAM);
        if (scratch == nullptr) {
            Serial.println("[GEMINI] failed to allocate setup scratch buffer");
            return false;
        }
    }

    static String path;
    path  = GEMINI_PATH;
    path += "?key=";
    path += GEMINI_API_KEY;

    ws.begin(GEMINI_HOST, GEMINI_PORT, path.c_str(), GTS_ROOT_R1_PEM,
             gWsRxBuf, WS_RX_ASSEMBLY_BYTES);
    ws.onEvent(onWsEvent, nullptr);
    return true;
}

static bool sendSetup() {
    // Resume the previous conversation if we have a handle from a prior session.
    String resume = "\"sessionResumption\":{}";
    if (sResumeHandle.length() > 0) {
        resume = "\"sessionResumption\":{\"handle\":\"" + sResumeHandle + "\"}";
    }

    // The live clock, appended to the compile-time prompt. Sessions recycle every
    // SESSION_RECYCLE_MS, so each new setup refreshes it and the model's idea of
    // "now" is never more than a few minutes old. Omitted entirely while SNTP is
    // still landing: no time at all is recoverable, a wrong one is not.
    char timeNote[160] = "";
    char stamp[64];
    if (timeNowLocal(stamp, sizeof(stamp))) {
        snprintf(timeNote, sizeof(timeNote),
                 TIME_PROMPT_PREFIX "%s" TIME_PROMPT_SUFFIX, stamp);
    } else {
        Serial.println("[GEMINI] clock not synced — sending prompt without a current time");
    }

    int n = snprintf(scratch, SCRATCH_BYTES,
        "{\"setup\":{"
          "\"model\":\"%s\","
          "\"generationConfig\":{"
            "\"responseModalities\":[\"AUDIO\"],"
            "\"speechConfig\":{\"voice_config\":{\"prebuilt_voice_config\":"
              "{\"voice_name\":\"%s\"}}}"
          "},"
          "\"systemInstruction\":{\"parts\":[{\"text\":\"%s%s\"}],\"role\":\"user\"},"
          "\"inputAudioTranscription\":{},"
          "\"outputAudioTranscription\":{},"
          "\"realtimeInputConfig\":{\"automatic_activity_detection\":{"
            "\"start_of_speech_sensitivity\":\"%s\","
            "\"end_of_speech_sensitivity\":\"%s\","
            "\"prefix_padding_ms\":%d,"
            "\"silence_duration_ms\":%d}},"
          "\"contextWindowCompression\":{\"triggerTokens\":104857,"
            "\"slidingWindow\":{\"targetTokens\":52428}},"
          "\"tools\":[{\"functionDeclarations\":[{"
            "\"name\":\"" CAM_TOOL_NAME "\","
            "\"description\":\"Take one still photo with the assistant camera and "
              "add it to the conversation. Use only when answering requires seeing "
              "something in the physical world right now. Permission is enforced "
              "outside your control: the first call always returns "
              "permission_required and takes no photo, and you must then ask the "
              "user out loud and wait for their answer before calling again.\","
            "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{}}"
          "},{"
            "\"name\":\"" MUSIC_TOOL_NAME "\","
            "\"description\":\"Play a song out loud on the assistant speaker. Give "
              "the song name the user asked for, plus the artist only if they named "
              "one, as plain words with no punctuation, quotes or URL. The device "
              "looks the song up itself and plays the closest match. The lookup takes "
              "a few seconds, so this returns 'searching' immediately and does not "
              "mean the song was found; if it cannot be found or played you will be "
              "told in a following message. Calling this replaces whatever is "
              "currently playing.\","
            "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
              "\"query\":{\"type\":\"STRING\",\"description\":\"Song name, "
                "optionally followed by the artist. Example: gehra hua\"}},"
              "\"required\":[\"query\"]}"
          "},{"
            "\"name\":\"" MUSIC_STOP_TOOL_NAME "\","
            "\"description\":\"Stop the song that is currently playing. Use only "
              "when the user asks to stop, pause or turn the music off. Speaking to "
              "the assistant already interrupts playback on its own, so this is not "
              "needed merely because the user said something.\","
            "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{}}"
          "},{"
            "\"name\":\"" NEWS_TOOL_NAME "\","
            "\"description\":\"Look up recent news headlines about a topic. Give "
              "the topic in a few plain words, no punctuation or URL. The device "
              "looks it up itself against Google News. The lookup takes a few "
              "seconds, so this returns 'searching' immediately and does not mean "
              "headlines were found; they are given in a following message once "
              "the lookup finishes.\","
            "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
              "\"query\":{\"type\":\"STRING\",\"description\":\"News topic, e.g. "
                "flood bihar\"}},"
              "\"required\":[\"query\"]}"
          "},{"
            "\"name\":\"" HOLIDAY_TOOL_NAME "\","
            "\"description\":\"Look up the next few upcoming public holidays for an "
              "Indian state. The device looks it up itself. The lookup takes a few "
              "seconds, so this returns 'searching' immediately and does not mean "
              "results were found; they are given in a following message once the "
              "lookup finishes.\","
            "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
              "\"state\":{\"type\":\"STRING\",\"description\":\"Indian state name in "
                "English, e.g. bihar, delhi, uttar pradesh. Omit to use the default "
                "state.\"}}}"
          "}]}],"
          "%s"
        "}}",
        GEMINI_MODEL, GEMINI_VOICE, SYSTEM_INSTRUCTION, timeNote,
        VAD_START_SENSITIVITY, VAD_END_SENSITIVITY,
        VAD_PREFIX_PADDING_MS, VAD_SILENCE_DURATION_MS,
        resume.c_str());

    if (n <= 0 || (size_t)n >= SCRATCH_BYTES) {
        Serial.printf("[GEMINI] setup message truncated (%d >= %u)\n",
                      n, (unsigned)SCRATCH_BYTES);
        return false;
    }

    Serial.printf("[GEMINI] --> setup request: model=%s voice=%s resume=%s time=%s\n",
                  GEMINI_MODEL, GEMINI_VOICE,
                  sResumeHandle.length() > 0 ? "yes" : "no",
                  timeNote[0] ? stamp : "unsynced");
    return ws.sendText(scratch, (size_t)n);
}

bool geminiConnect() {
    sSetupComplete = false;
    sGoAway        = false;

    if (!ws.connect()) {
        return false;
    }

    if (!sendSetup()) {
        Serial.println("[GEMINI] failed to send setup");
        ws.disconnect();
        return false;
    }

    // Wait for setupComplete before letting the mic stream start.
    uint32_t start = millis();
    while (!sSetupComplete && millis() - start < 10000) {
        if (!ws.poll(50)) {
            Serial.println("[GEMINI] disconnected while waiting for setupComplete");
            return false;
        }
        delay(5);
    }

    if (!sSetupComplete) {
        Serial.println("[GEMINI] timed out waiting for setupComplete");
        ws.disconnect();
        return false;
    }

    return true;
}

void geminiDisconnect() {
    ws.disconnect();
    sSetupComplete = false;
}

bool geminiIsConnected() {
    return ws.isConnected() && sSetupComplete;
}

bool geminiPoll(uint32_t maxMs) {
    return ws.poll(maxMs);
}

bool geminiSendRaw(const char* json, size_t len) {
    return ws.sendText(json, len);
}

bool geminiBuildAudioMessage(const int16_t* pcm, size_t samples,
                             char* dst, size_t cap, size_t* outLen) {
    static const char PRE[]  = "{\"realtimeInput\":{\"audio\":{\"data\":\"";
    // Rate is stringified from MIC_SAMPLE_RATE so the two can never drift apart.
    static const char POST[] = "\",\"mimeType\":\"audio/pcm;rate=" TOSTR(MIC_SAMPLE_RATE) "\"}}}";

    const size_t preLen  = sizeof(PRE) - 1;
    const size_t postLen = sizeof(POST) - 1;
    const size_t rawBytes = samples * sizeof(int16_t);

    if (preLen + b64EncodedLen(rawBytes) + postLen > cap) {
        return false;
    }

    memcpy(dst, PRE, preLen);

    size_t enc = 0;
    if (!b64Encode((const uint8_t*)pcm, rawBytes, dst + preLen, cap - preLen, &enc)) {
        return false;
    }

    // b64Encode NUL-terminates; POST overwrites that byte.
    memcpy(dst + preLen + enc, POST, postLen);
    *outLen = preLen + enc + postLen;
    return true;
}

/** Reply to a tool call. `result` must be a bare identifier — see isSafeJsonToken. */
static bool sendToolResponse(const char* id, const char* name, const char* result) {
    int n = snprintf(scratch, SCRATCH_BYTES,
        "{\"toolResponse\":{\"functionResponses\":[{"
          "\"id\":\"%s\",\"name\":\"%s\",\"response\":{\"result\":\"%s\"}"
        "}]}}",
        id, name, result);

    if (n <= 0 || (size_t)n >= SCRATCH_BYTES) {
        return false;
    }
    Serial.printf("[GEMINI] --> toolResponse %s: %s\n", name, result);
    return ws.sendText(scratch, (size_t)n);
}

/**
 * Tell the model something the user did not say, and make it speak.
 *
 * Sent as a user turn with turnComplete because that is what prompts a reply; a
 * system-role part would land in context silently and RIO would never actually
 * pass this on. `text` is free-form and is escaped on the way out.
 *
 * 600 rather than 320: news notices carry up to NEWS_HEADLINES_MAX of actual
 * headline text wrapped in a sentence, on top of the topic itself.
 */
static bool sendModelNote(const char* text) {
    char esc[600];
    if (!jsonEscape(text, esc, sizeof(esc))) {
        return false;
    }

    int n = snprintf(scratch, SCRATCH_BYTES,
        "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":"
          "\"[system note, not spoken by the user] %s Tell him briefly.\"}]}],"
        "\"turnComplete\":true}}",
        esc);

    if (n <= 0 || (size_t)n >= SCRATCH_BYTES) {
        return false;
    }
    return ws.sendText(scratch, (size_t)n);
}

bool geminiServicePendingToolCall() {
    if (!sHasPendingCall) {
        return false;
    }
    sHasPendingCall = false;

    const char* id   = sPendingCallId;
    const char* name = sPendingCallName;

    // --- Music ---------------------------------------------------------------
    // Answered immediately and serviced elsewhere. A search is seconds and a
    // song is minutes; netTask must go back to polling the WebSocket long before
    // either finishes, so musicTask takes the work and any failure comes back
    // later through musicTakeNotice().
    if (strcmp(name, MUSIC_TOOL_NAME) == 0) {
        if (sPendingCallQuery[0] == '\0') {
            sendToolResponse(id, name, "no_query");
            return true;
        }
        if (!musicRequest(sPendingCallQuery)) {
            sendToolResponse(id, name, "unavailable");
            return true;
        }
        oledTriggerTool(OLED_TOOL_MUSIC);
        oledSetMode(OLED_MODE_PROCESSING);   // lookup + CDN fetch, seconds long
        sendToolResponse(id, name, "searching");
        return true;
    }

    if (strcmp(name, MUSIC_STOP_TOOL_NAME) == 0) {
        const bool wasPlaying = musicIsActive();
        musicStop();
        sendToolResponse(id, name, wasPlaying ? "stopped" : "nothing_playing");
        return true;
    }

    // --- News ------------------------------------------------------------
    // Same shape as music: answered "searching" immediately, and the actual
    // headlines (or a failure) come back later through geminiPumpNewsNotices().
    if (strcmp(name, NEWS_TOOL_NAME) == 0) {
        if (sPendingCallQuery[0] == '\0') {
            sendToolResponse(id, name, "no_query");
            return true;
        }
        if (!newsRequest(sPendingCallQuery)) {
            sendToolResponse(id, name, "unavailable");
            return true;
        }
        oledSetMode(OLED_MODE_PROCESSING);
        sendToolResponse(id, name, "searching");
        return true;
    }

    // --- Holidays ----------------------------------------------------------
    // Same shape as news, except an empty query is not an error — it just
    // means holidayRequest() falls back to HOLIDAY_DEFAULT_STATE.
    if (strcmp(name, HOLIDAY_TOOL_NAME) == 0) {
        if (!holidayRequest(sPendingCallQuery)) {
            sendToolResponse(id, name, "unavailable");
            return true;
        }
        oledSetMode(OLED_MODE_PROCESSING);
        sendToolResponse(id, name, "searching");
        return true;
    }

    if (strcmp(name, CAM_TOOL_NAME) != 0) {
        sendToolResponse(id, name, "unknown_tool");
        return true;
    }

    // An answer that never came within the window is not consent for a request
    // made much later.
    if (sGate == GATE_AWAITING_ANSWER
        && millis() - sGateArmedAt > CAM_CONSENT_WINDOW_MS) {
        Serial.println("[CAM] consent window expired — asking again");
        sGate = GATE_IDLE;
    }

    // Phase 1: refuse, and make the model ask. The camera is not touched.
    if (sGate == GATE_IDLE) {
        sGate              = GATE_AWAITING_ANSWER;
        sGateArmedAt       = millis();
        sGateArmedModelSeq = sModelTurnSeq;
        Serial.println("[CAM] permission requested — camera stays off until the user answers");
        sendToolResponse(id, name, "permission_required");
        return true;
    }

    // Phase 2. Two things must have happened since the refusal, in order: the
    // model finished a turn (it asked), and the user then began one (they
    // answered). Either alone is not consent.
    const bool modelAsked  = sModelTurnSeq > sGateArmedModelSeq;
    const bool userAnswered = sUserTurnSeq > sUserSeqAtTurnEnd;

    if (!modelAsked || !userAnswered) {
        Serial.printf("[CAM] still refused (asked=%d answered=%d)\n",
                      (int)modelAsked, (int)userAnswered);
        sendToolResponse(id, name, "permission_required");
        return true;
    }

    sGate = GATE_IDLE;
    Serial.println("[CAM] permission granted — powering up sensor");
    oledTriggerTool(OLED_TOOL_CAPTURE);
    // The capture pipeline runs ~1.5-4.5 s (sensor bring-up, metering, shutter),
    // far longer than the 1.4 s overlay. Without this the eyes would sit in
    // LISTENING through several seconds of very obvious work.
    oledSetMode(OLED_MODE_PROCESSING);

    size_t len = 0;
    if (!cameraCaptureToMessage(&len)) {
        sendToolResponse(id, name, "capture_failed");
        return true;
    }

    // Order matters, and it is the opposite of what it looks like it should be.
    //
    // The photo goes out as its own clientContent user turn (see the note in
    // cameraCaptureToMessage). Opening that turn while the function call is still
    // outstanding was measured against the live endpoint and the model went
    // silent for the rest of the exchange — it never answered at all. Answering
    // the call first and letting the photo follow as the next complete turn is
    // what actually gets RIO to describe it.
    if (!sendToolResponse(id, name, "image_captured")) {
        Serial.println("[CAM] failed to answer the tool call");
        return true;
    }

    if (!ws.sendText(gImgTxBuf, len)) {
        // The model has already been told the photo exists and there is no way to
        // retract that, so say so out loud instead: the alternative is RIO
        // confidently describing a frame she was never given.
        Serial.println("[CAM] failed to send frame");
        sendModelNote("The photo was taken but could not be sent, so you "
                      "cannot see it.");
    }
    return true;
}

bool geminiPumpMusicNotices() {
    char notice[160];
    if (!musicTakeNotice(notice, sizeof(notice))) {
        return false;
    }

    // Only failures ever arrive here, so this always has something worth saying.
    Serial.printf("[MUSIC] --> notifying model: %s\n", notice);
    return sendModelNote(notice);
}

bool geminiPumpNewsNotices() {
    char notice[NEWS_HEADLINES_MAX + 96];
    if (!newsTakeNotice(notice, sizeof(notice))) {
        return false;
    }

    Serial.printf("[NEWS] --> notifying model: %s\n", notice);
    return sendModelNote(notice);
}

bool geminiPumpHolidayNotices() {
    char notice[HOLIDAYS_TEXT_MAX + 96];
    if (!holidayTakeNotice(notice, sizeof(notice))) {
        return false;
    }

    Serial.printf("[HOLIDAY] --> notifying model: %s\n", notice);
    return sendModelNote(notice);
}

bool geminiShouldRecycle() {
    return sGoAway || ws.connectedForMs() > SESSION_RECYCLE_MS;
}
