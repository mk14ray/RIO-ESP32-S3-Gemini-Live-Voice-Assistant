// =============================================================================
// RIO — standalone realtime voice assistant
//
// Seeed Studio XIAO ESP32-S3 Sense + MAX98357A, talking directly to the
// Google Gemini Live API over wss://. No PC involved at runtime.
//
//   onboard PDM mic (I2S0, 16 kHz) --> base64/JSON --> Gemini Live
//   Gemini Live --> base64 PCM (24 kHz) --> MAX98357A (I2S1)
//
// Half-duplex: the mic is muted for the whole reply plus a short tail, because
// the speaker sits next to the mic with no echo cancellation.
//
// See README.md for wiring, Arduino IDE settings, and the bring-up procedure.
// =============================================================================

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "app_state.h"
#include "audio_in.h"
#include "audio_out.h"
#include "aec.h"
#include "wifi_mgr.h"
#include "gemini_live.h"
#include "music.h"
#include "news.h"
#include "holiday.h"
#include "memmon.h"
#include "oled_display.h"

// -----------------------------------------------------------------------------
// Bring-up modes. Set BRINGUP_MODE to isolate one subsystem at a time; a failure
// then points at a single layer instead of the whole stack. Leave at
// BRINGUP_NONE for normal operation.
// -----------------------------------------------------------------------------
#define BRINGUP_NONE      0   // full assistant
#define BRINGUP_TONE      1   // step 1: 440 Hz tone -> proves wiring + I2S1
#define BRINGUP_MIC       2   // step 2: log mic RMS -> proves I2S0 + tunes gain
#define BRINGUP_LOOPBACK  3   // step 3: mic -> speaker -> proves both directions
#define BRINGUP_AEC       4   // step 4: measure ERLE -> calibrates AEC_REF_DELAY_MS

#define BRINGUP_MODE  BRINGUP_NONE

// Chunks micTask declined to upload because they were pure gate output while RIO
// was speaking. Reported as muted= in [STAT]; both writer and reader live in
// this file. See MIC_SKIP_GATED_UPLOAD.
static std::atomic<uint32_t> sMutedChunks(0);

static TaskHandle_t hMic = NULL;
static TaskHandle_t hSpk = NULL;
static TaskHandle_t hNet = NULL;

static void ledSet(bool on) {
    digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);   // active LOW on the XIAO
}

/** Unrecoverable startup failure: blink fast forever rather than run half-built. */
static void halt(const char* why) {
    Serial.printf("\n[FATAL] %s\n", why);
    Serial.println("[FATAL] halted — fix the above and reset.");
    oledFatal("FATAL: %s", why);
    pinMode(STATUS_LED_PIN, OUTPUT);
    for (;;) {
        ledSet(true);  delay(100);
        ledSet(false); delay(100);
    }
}

// =============================================================================
// micTask — capture, gate, encode
// =============================================================================
#if FULL_DUPLEX && MIC_SKIP_GATED_UPLOAD
/**
 * True if the block is entirely zeroes.
 *
 * Exact zero is the gate's own signature — it memsets rejected frames rather
 * than attenuating them (see aecProcess) — so this cannot be confused with quiet
 * speech or a low noise floor, both of which dither around zero and never hold
 * it for 1600 consecutive samples.
 */
static bool allZero(const int16_t* pcm, size_t samples) {
    for (size_t i = 0; i < samples; i++) {
        if (pcm[i] != 0) {
            return false;
        }
    }
    return true;
}
#endif

/** Upload whatever has accumulated. Returns false if the frame was dropped. */
static bool micFlush(const int16_t* pcm, size_t samples) {
    uint8_t idx;
    if (xQueueReceive(gTxFree, &idx, pdMS_TO_TICKS(50)) != pdTRUE) {
        // Blocking longer would overrun the RX DMA. A counted drop is far
        // easier to diagnose than glitchy audio.
        gDroppedChunks.fetch_add(1);
        return false;
    }

    char*  slot = (char*)(gTxSlots + (size_t)idx * TX_SLOT_BYTES);
    size_t len  = 0;
    if (!geminiBuildAudioMessage(pcm, samples, slot, TX_SLOT_BYTES, &len)) {
        // Distinct from a queue-full drop and worth saying out loud: this one is
        // a sizing mistake, not congestion, and it drops 100% of audio rather
        // than a burst. Lumping both into gDroppedChunks hides that completely.
        static uint32_t lastGripe = 0;
        if (millis() - lastGripe > 5000) {
            lastGripe = millis();
            Serial.printf("[MIC] encode failed: %u samples do not fit TX_SLOT_BYTES=%u\n",
                          (unsigned)samples, (unsigned)TX_SLOT_BYTES);
        }
        xQueueSend(gTxFree, &idx, 0);
        gDroppedChunks.fetch_add(1);
        return false;
    }

    gTxSlotLen[idx] = len;
    if (xQueueSend(gTxReady, &idx, pdMS_TO_TICKS(50)) != pdTRUE) {
        xQueueSend(gTxFree, &idx, 0);
        gDroppedChunks.fetch_add(1);
        return false;
    }
    return true;
}

static void micTask(void* arg) {
    (void)arg;

    // In full duplex the AEC dictates the capture granularity: it cancels one
    // fixed-size frame at a time. Those frames are accumulated back up to
    // MIC_CHUNK_MS before upload, so the network still sees the same 100 ms
    // chunks it did before and only the internal cadence changes.
#if FULL_DUPLEX
    const size_t frame = aecFrameSamples();
#else
    const size_t frame = MIC_CHUNK_SAMPLES;
#endif

    // Upload granularity: exactly MIC_CHUNK_SAMPLES, which is what TX_SLOT_BYTES
    // was sized against.
    //
    // Do NOT derive this from `frame`. The AEC consumes whole `frame`s but does
    // not EMIT them: aecProcess() re-blocks its output into noise-suppressor
    // frames and carries the remainder, so it returns 480 or 640 samples for a
    // 512-sample input. Rounding the threshold down to a multiple of `frame`
    // (1536) and then flushing the whole accumulator therefore overshot on a
    // fixed 480,480,480,480,640 cycle: two chunks of 1600 followed by one of
    // 1920, and 1920 samples base64 to 5194 B against a 5120 B slot. One upload
    // in three — 37% of all captured audio — was silently discarded, punching
    // 120 ms holes in the speech handed to Gemini and wrecking transcription.
    //
    // The accumulator is drained in exact flushAt-sized units instead, so the
    // encoded size is constant no matter what granularity the front end emits.
    const size_t flushAt = MIC_CHUNK_SAMPLES;

    // Accumulator holds a full chunk plus one front-end output, so the last
    // block can always be written whole before the flush test runs.
#if FULL_DUPLEX
    const size_t accumCap = flushAt + aecMaxOutputSamples();
#else
    const size_t accumCap = flushAt + frame;
#endif

#if FULL_DUPLEX
    const size_t cleanCap = aecMaxOutputSamples();
#else
    const size_t cleanCap = frame;
#endif

    int16_t* raw   = (int16_t*)heap_caps_malloc(frame * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t* clean = (int16_t*)heap_caps_malloc(cleanCap * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    int16_t* accum = (int16_t*)heap_caps_malloc(accumCap * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    if (raw == nullptr || clean == nullptr || accum == nullptr) {
        Serial.println("[MIC] buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }

    Serial.printf("[MIC] frame=%u samples, upload chunk=%u samples (%u ms), "
                  "encodes to ~%u of %u slot bytes\n",
                  (unsigned)frame, (unsigned)flushAt,
                  (unsigned)(flushAt * 1000 / MIC_SAMPLE_RATE),
                  (unsigned)(35 + 4 * ((flushAt * 2 + 2) / 3) + 38),
                  (unsigned)TX_SLOT_BYTES);

    size_t   accumN    = 0;
    uint32_t lastRmsLog = 0;
#if !FULL_DUPLEX
    uint8_t  lastState = CONV_BOOT;
#endif

    for (;;) {
        // The AEC needs exactly one frame, so short reads are stitched rather
        // than discarded — a partial frame dropped here would shift the mic and
        // reference streams permanently out of alignment.
        size_t got = 0;
        while (got < frame) {
            size_t n = audioInRead(raw + got, frame - got, 200);
            if (n == 0) {
                break;
            }
            got += n;
        }
        if (got < frame) {
            continue;
        }

#if FULL_DUPLEX
        // The mic is never muted. RIO's own voice is cancelled, the room is
        // de-noised, and anything still indistinguishable from echo is gated to
        // silence — which is what lets the user talk over a reply without RIO
        // answering itself.
        AecStatus st;
        const size_t cleanN = aecProcess(raw, clean, &st);
        if (cleanN == 0) {
            continue;               // all of it was carried to the next frame
        }
#else
        const size_t cleanN = frame;
        // Half duplex: discard everything captured while the speaker is live.
        const uint8_t st = gConv.load();
        if (st != CONV_LISTENING) {
            lastState = st;
            accumN = 0;
            continue;
        }
        if (lastState != CONV_LISTENING) {
            audioInDrain();                 // entering LISTENING: drop stale audio
            lastState = CONV_LISTENING;
            accumN = 0;
            continue;
        }
        memcpy(clean, raw, frame * sizeof(int16_t));
#endif

        audioApplyGain(clean, cleanN, MIC_GAIN_FACTOR);

        memcpy(accum + accumN, clean, cleanN * sizeof(int16_t));
        accumN += cleanN;

        // Level logging: the single most useful number when tuning
        // MIC_GAIN_FACTOR, and in full duplex the quickest way to see whether
        // the front end is holding up while RIO talks. `gate=shut` while RIO
        // speaks is the system correctly refusing to hear itself.
#if LOG_MIC_LEVEL
        if (millis() - lastRmsLog > 2000) {
            lastRmsLog = millis();
#if FULL_DUPLEX
            Serial.printf("[MIC] rms=%.0f erle=%.1fdB %s%s\n",
                          audioRms(clean, cleanN), aecErleDb(),
                          st.playing ? "rio=speaking " : "",
                          st.gated ? "gate=shut" : (st.speech ? "gate=open speech" : "gate=open"));
#else
            Serial.printf("[MIC] rms=%.0f\n", audioRms(clean, cleanN));
#endif
        }
#else
        (void)lastRmsLog;
#endif

        // Drain in whole chunks and keep the tail. Resetting accumN to 0 here
        // would throw the remainder away and reintroduce the gaps this loop
        // exists to close.
        while (accumN >= flushAt) {
#if FULL_DUPLEX && MIC_SKIP_GATED_UPLOAD
            // st.playing carries the gate's hangover, so this stays true across
            // the reverberant tail after the last sample is written — the window
            // where a chunk can still be pure echo.
            const bool muted = st.playing && allZero(accum, flushAt);
            if (muted) {
                sMutedChunks.fetch_add(1);
            }
            if (!muted) {
                micFlush(accum, flushAt);
            }
#else
            micFlush(accum, flushAt);
#endif
            accumN -= flushAt;
            if (accumN > 0) {
                memmove(accum, accum + flushAt, accumN * sizeof(int16_t));
            }
        }
    }
}

// =============================================================================
// spkTask — playback and the half-duplex tail
// =============================================================================
#define SPK_CHUNK_SAMPLES 512

/**
 * Hand the floor back once the reply has finished playing.
 *
 * Split out of the task body because there are now two paths that reach "there
 * is nothing left to play": the drained-read path, and the jitter buffer idling
 * on an empty gPcmOut. Both have to be able to end the turn, or a reply that
 * ends exactly on a buffer boundary would leave RIO stuck in SPEAKING.
 *
 * Safe to call while priming only because gTurnComplete forces the prebuffer to
 * release (see spkTask): whenever this runs, anything still buffered has
 * already been played out.
 */
static void spkEndTurnIfDone() {
    if (gConv.load() != CONV_SPEAKING || !gTurnComplete.load()) {
        return;
    }

    uint8_t expect = CONV_SPEAKING;
#if FULL_DUPLEX
    // No tail: the mic was never closed, so there is nothing to reopen and
    // nothing to wait out. SPEAKING is now only a label for the LED and the
    // session-recycle check.
    if (gConv.compare_exchange_strong(expect, CONV_LISTENING)) {
        gTurnComplete.store(false);
        Serial.println("[STATE] listening");
        oledSetMode(OLED_MODE_LISTENING);
    }
#else
    if (gConv.compare_exchange_strong(expect, CONV_TAIL)) {
        // Writing real silence (rather than sleeping) both flushes the last
        // samples through the DMA and provides the acoustic tail.
        audioOutWriteSilence(SPEAK_TAIL_MS);
        gTurnComplete.store(false);

        expect = CONV_TAIL;
        gConv.compare_exchange_strong(expect, CONV_LISTENING);
        Serial.println("[STATE] listening");
        oledSetMode(OLED_MODE_LISTENING);
    }
#endif
}

/**
 * Drop the amplifier into shutdown once playback has genuinely finished.
 *
 * Four conditions, and each one is load-bearing:
 *
 *  - already off: nothing to do, and skips the rest on every idle poll.
 *  - not SPEAKING and no song playing: a mid-reply underrun or a stalled stream
 *    must not produce a mute/unmute click in the middle of the audio. The turn
 *    state, not the buffer level, is what says the reply is over.
 *  - gPcmOut empty: nothing is waiting that a mute would cut off.
 *  - AMP_MUTE_LINGER_MS since the last write: i2s_channel_write() returns when
 *    the DMA accepts a buffer, not when the amplifier has played it, so ~60 ms
 *    of accepted audio is still queued at the moment the last write returns.
 *
 * Note what is deliberately absent: this never mutes while samples are flowing,
 * so the acoustic path the canceller has converged on does not change underneath
 * it. Muting only ever happens in a silence the AEC is not being asked to model.
 */
static void spkAmpIdleCheck(uint32_t lastWriteMs) {
    if (!audioOutAmpIsOn()) {
        return;
    }
    if (gConv.load() == CONV_SPEAKING || musicIsPlaying()) {
        return;
    }
    if (xStreamBufferIsEmpty(gPcmOut) != pdTRUE) {
        return;
    }
    if (millis() - lastWriteMs < AMP_MUTE_LINGER_MS) {
        return;
    }
    audioOutAmpSet(false);
}

static void spkTask(void* arg) {
    (void)arg;

    uint8_t* buf = (uint8_t*)heap_caps_malloc(SPK_CHUNK_SAMPLES * sizeof(int16_t),
                                              MALLOC_CAP_INTERNAL);
    if (buf == nullptr) {
        Serial.println("[SPK] chunk buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }

    // gPcmOut is a raw byte stream fed by base64-decoded chunks of arbitrary
    // (multiple-of-3) size, so a read from it can end mid-sample. Carry a
    // stray trailing byte forward instead of dropping it: dropping it would
    // shift every following sample one byte out of phase, which is audible
    // as static/garbled playback for the rest of the stream.
    bool    haveOddByte = false;
    uint8_t oddByte     = 0;

    // Jitter buffer state. `primed` is false whenever playback is stopped and
    // waiting for enough audio to pile up; `primeStart` timestamps the wait so
    // it can be abandoned rather than hung on forever.
    bool     primed     = false;
    uint32_t primeStart = 0;

    // Timestamp of the last sample handed to I2S — the linger that keeps the
    // amplifier live long enough for the TX DMA to drain runs off this.
    // Starts at 0 with the amp already shut down, so the first idle check exits
    // on audioOutAmpIsOn() and the initial value is never actually compared.
    uint32_t lastWriteMs = 0;

    for (;;) {
        // The reset happens here, not in netTask: resetting a stream buffer
        // while another task is blocked on it is undefined.
        if (gFlushPlayback.exchange(false)) {
            xStreamBufferReset(gPcmOut);
            haveOddByte = false;   // belongs to the flushed audio — discard it too
            primed      = false;   // whatever was buffered is gone; re-prime
            primeStart  = 0;
#if FULL_DUPLEX
            // The discarded audio is never going to reach the speaker, so the
            // reference describing it must go too. Leaving it would have the
            // canceller subtracting an echo that no longer exists, right at the
            // moment the user is talking.
            aecFlushReference();
#endif
        }

        // --- Jitter buffer ---------------------------------------------------
        // Let PLAYBACK_PREBUFFER_MS of reply accumulate before the first sample
        // reaches the amplifier. Without this the speaker runs level with the
        // network and the 60 ms DMA queue is the entire tolerance for a late
        // WebSocket message — which is what made replies break up mid-word.
        if (!primed) {
            const size_t have = xStreamBufferBytesAvailable(gPcmOut);

            if (have >= PLAYBACK_PREBUFFER_BYTES) {
                primed = true;
            } else if (have > 0) {
                if (primeStart == 0) {
                    primeStart = millis();
                }
                // Release early for anything that is never going to reach the
                // fill level: a reply that is simply short (turnComplete has
                // already arrived), or one trickling in over a bad link.
                if (gTurnComplete.load()
                    || millis() - primeStart >= PLAYBACK_PREBUFFER_MAX_WAIT_MS) {
                    primed = true;
                }
            } else {
                primeStart = 0;     // nothing in flight; not actually waiting
            }

            if (!primed) {
                // Reachable with audio still buffered only while gTurnComplete
                // is false, and spkEndTurnIfDone() does nothing in that case.
                spkEndTurnIfDone();
                // The idle loop for the entire time RIO is listening, which is
                // where the amp spends most of its life and therefore where the
                // mute has to be applied.
                spkAmpIdleCheck(lastWriteMs);
                // Poll tightly only while a reply is actually filling up, so
                // the prebuffer is released the moment it is ready. With an
                // empty buffer there is nothing to watch and this is the idle
                // loop for the whole time RIO is listening — back off, or a
                // priority-6 task spins against micTask's canceller for
                // nothing.
                vTaskDelay(pdMS_TO_TICKS(have > 0 ? 5 : 20));
                continue;
            }
        }

        size_t off = 0;
        if (haveOddByte) {
            buf[0]      = oddByte;
            off         = 1;
            haveOddByte = false;
        }

        size_t got = xStreamBufferReceive(gPcmOut, buf + off,
                                          SPK_CHUNK_SAMPLES * sizeof(int16_t) - off,
                                          pdMS_TO_TICKS(50));
        size_t total = got + off;

        if (total >= sizeof(int16_t)) {
            const size_t evenBytes = total & ~(size_t)1;
            const size_t want      = evenBytes / sizeof(int16_t);

            // Ahead of the reference push, not after it. On a rising edge this
            // blocks for AMP_UNMUTE_SETTLE_MS, and pushing the reference first
            // would tell the canceller these samples reached the room several
            // milliseconds before the amplifier was actually driving — a delay
            // error at the top of every turn, in the one place the filter has
            // just been asked to re-converge.
            audioOutAmpSet(true);
            lastWriteMs = millis();
#if FULL_DUPLEX
            // Recorded immediately before the write, so the delay line receives
            // exactly the samples the amplifier does, in the same order. Any
            // divergence here shows up directly as lost cancellation.
            aecPushReference((const int16_t*)buf, want);
#endif
            const size_t wrote = audioOutWriteMono((int16_t*)buf, want, 1000);
            if (wrote < want) {
                // Dropped samples are an audible break, and in full duplex they
                // also leave the AEC reference describing audio the amplifier
                // never played. Silent truncation would make both look like a
                // network problem, so say it out loud.
                static uint32_t lastShort = 0;
                if (millis() - lastShort > 5000) {
                    lastShort = millis();
                    Serial.printf("[SPK] short write: %u of %u samples reached I2S\n",
                                  (unsigned)wrote, (unsigned)want);
                }
                gUnderruns.fetch_add(1);
            }
            if (total != evenBytes) {
                oddByte     = buf[evenBytes];
                haveOddByte = true;
            }
            continue;
        }
        if (total == 1) {
            // Nothing new arrived within the timeout; keep holding the byte.
            oddByte     = buf[0];
            haveOddByte = true;
        }

        // Buffer drained while primed. If the reply is not over, the speaker has
        // caught up with the network: count the break and re-prime, so one late
        // message costs a single gap instead of a run of stuttering ones.
        primed     = false;
        primeStart = 0;
        if (!gTurnComplete.load() && gConv.load() == CONV_SPEAKING) {
            gUnderruns.fetch_add(1);
        }

        spkEndTurnIfDone();
        // Second of the two paths that reach "nothing left to play". The reply
        // that ends exactly on a buffer boundary comes through here rather than
        // through the prime loop above, so the mute has to be tested in both.
        spkAmpIdleCheck(lastWriteMs);
    }
}

// =============================================================================
// netTask — sole owner of the WebSocket
// =============================================================================
static void netTask(void* arg) {
    (void)arg;

    uint32_t backoff = 1000;

    for (;;) {
        if (!wifiIsConnected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!geminiIsConnected()) {
            gConv.store(CONV_CONNECTING);
            gTurnComplete.store(false);
            gFlushPlayback.store(true);   // spkTask clears the stale reply buffer

            if (geminiConnect()) {
                backoff = 1000;
                gConv.store(CONV_LISTENING);
                Serial.println("[STATE] listening — say something");
                oledSetMode(OLED_MODE_LISTENING);
            } else {
                gReconnects.fetch_add(1);
                // +-20% jitter so repeated failures do not synchronise.
                uint32_t jitter = backoff / 5;
                uint32_t wait   = backoff - jitter + (esp_random() % (2 * jitter + 1));
                Serial.printf("[NET] reconnect in %u ms\n", (unsigned)wait);
                vTaskDelay(pdMS_TO_TICKS(wait));
                backoff = (backoff * 2 > 30000) ? 30000 : backoff * 2;
                continue;
            }
        }

        // Drain the outbound queue.
        uint8_t idx;
        while (xQueueReceive(gTxReady, &idx, 0) == pdTRUE) {
            const char* slot = (const char*)(gTxSlots + (size_t)idx * TX_SLOT_BYTES);
            bool ok = geminiSendRaw(slot, gTxSlotLen[idx]);
            xQueueSend(gTxFree, &idx, 0);
            if (!ok) {
                break;
            }
        }

        geminiPoll(20);

        // Serviced here rather than inside the poll callback: a granted capture
        // powers up the camera and blocks for 1.5-2 s while the sensor converges
        // and the shutter picks its sharpest frame, and that cost belongs on
        // netTask's own schedule rather than buried in a parser callback.
        //
        // That block is longer than the ~600 ms of audio the TX slots hold, so a
        // capture drops roughly a second of mic input and bumps `drops=` in the
        // [STAT] line. That is accounted for, not overlooked: the drop path is
        // counted and lossy rather than blocking, so nothing desyncs, and the
        // audio being dropped is the second immediately after the user has said
        // "yes, take a look" — which is the least costly second in the whole
        // conversation to lose. Shorten it by lowering CAM_SHOT_CANDIDATES or
        // CAM_TUNE_BUDGET_MS if that trade ever stops being worth it.
        geminiServicePendingToolCall();

        // Drained here rather than from musicTask, which does not own the
        // WebSocket. A song that failed to resolve or stalled mid-stream is
        // only ever reported this way — without this call the model promises
        // to tell him and then never does.
        geminiPumpMusicNotices();

        // Same reasoning: newsTask does not own the WebSocket either, so a
        // finished lookup is only ever reported this way.
        geminiPumpNewsNotices();

        geminiPumpHolidayNotices();

        // Recycle the session only at a turn boundary, never mid-reply.
        if (geminiShouldRecycle()
            && gConv.load() == CONV_LISTENING
            && xStreamBufferIsEmpty(gPcmOut) == pdTRUE) {
            Serial.println("[NET] recycling session");
            geminiDisconnect();
            gReconnects.fetch_add(1);
        }

        vTaskDelay(1);
    }
}

// =============================================================================
// setup / loop
// =============================================================================
void setup() {
    Serial.begin(115200);
    uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 3000) {
        delay(10);
    }
    delay(500);

    pinMode(STATUS_LED_PIN, OUTPUT);
    ledSet(false);

    // As early as possible. AMP_SD_PIN is U0RXD and its level before this runs
    // is not guaranteed, so until the pin is claimed the amplifier is enabled by
    // whatever the breakout's divider and the ROM leave on it.
    audioOutAmpBegin();

    // Non-fatal: RIO runs fine with no display attached. oledLog() is then
    // simply a no-op everywhere else it's called below.
    if (!oledBegin()) {
        Serial.println("[OLED] not found — continuing without display");
    }

    Serial.println();
    Serial.println("=================================================");
    Serial.println(" RIO — Gemini Live voice assistant");
    Serial.println(" XIAO ESP32-S3 Sense + MAX98357A");
    Serial.println("=================================================");
    oledLog("RIO booting...");

    // Checked first, before anything allocates. The echo canceller and the
    // camera both draw on PSRAM and neither fails politely without it — the
    // canceller faults inside its own init — so discovering this here turns a
    // boot loop with no explanation into one readable line.
    if (!psramFound() || ESP.getFreePsram() == 0) {
        halt("no PSRAM — set Tools > PSRAM to \"OPI PSRAM\"");
    }
    Serial.printf("[BOOT] PSRAM %u bytes free\n", (unsigned)ESP.getFreePsram());

    if (!audioOutBegin()) {
        halt("speaker (I2S1) init failed — check BCLK/LRC/DIN wiring");
    }

#if BRINGUP_MODE == BRINGUP_TONE
    Serial.println("[BRINGUP] playing 440 Hz tone every 2 s");
    for (;;) {
        audioOutTestTone(440, 500);
        delay(1500);
    }
#endif

    if (!audioInBegin()) {
        halt("microphone (I2S0 PDM) init failed");
    }

#if BRINGUP_MODE == BRINGUP_MIC
    Serial.println("[BRINGUP] logging mic RMS — speak, and watch the numbers");
    {
        static int16_t buf[MIC_CHUNK_SAMPLES];   // static: 3.2 KB is too much for the loop stack
        for (;;) {
            size_t n = audioInRead(buf, MIC_CHUNK_SAMPLES, 500);
            if (n > 0) {
                audioApplyGain(buf, n, MIC_GAIN_FACTOR);
                Serial.printf("[BRINGUP] samples=%u rms=%.0f\n",
                              (unsigned)n, audioRms(buf, n));
            }
        }
    }
#endif

#if BRINGUP_MODE == BRINGUP_LOOPBACK
    Serial.println("[BRINGUP] mic -> speaker loopback (expect howling: that is the echo");
    Serial.println("          coupling the half-duplex logic exists to defeat)");
    {
        static int16_t buf[MIC_CHUNK_SAMPLES];   // static: 3.2 KB is too much for the loop stack
        for (;;) {
            size_t n = audioInRead(buf, MIC_CHUNK_SAMPLES, 500);
            if (n > 0) {
                audioApplyGain(buf, n, MIC_GAIN_FACTOR);
                audioOutWriteMono(buf, n, 500);
            }
        }
    }
#endif

#if FULL_DUPLEX
    if (!aecBegin()) {
        halt("echo canceller init failed — full duplex cannot run without it");
    }
#endif

#if BRINGUP_MODE == BRINGUP_AEC
    Serial.println("[BRINGUP] AEC calibration — sweeping AEC_REF_DELAY_MS automatically.");
    Serial.println("          Noise plays for the whole sweep. STAY SILENT and do not move");
    Serial.println("          the board: your voice is measured as failed cancellation.");
    Serial.println("          Takes about a minute. Copy the winning value into config.h.");
    {
        // Broadband noise, not a tone: an adaptive filter converges on what it
        // is excited by, and a single sine tells you almost nothing about how
        // the canceller will behave on speech.
        xTaskCreatePinnedToCore([](void*) {
            static int16_t noise[256];
            for (;;) {
                for (size_t i = 0; i < 256; i++) {
                    noise[i] = (int16_t)((esp_random() & 0x3FFF) - 0x2000);
                }
                aecPushReference(noise, 256);
                audioOutWriteMono(noise, 256, 1000);
            }
        }, "noise", 4096, NULL, 6, NULL, 1);

        const size_t frame = aecFrameSamples();
        int16_t* raw   = (int16_t*)heap_caps_malloc(frame * 2, MALLOC_CAP_INTERNAL);
        int16_t* clean = (int16_t*)heap_caps_malloc(frame * 2, MALLOC_CAP_INTERNAL);
        if (raw == nullptr || clean == nullptr) {
            halt("bring-up buffer allocation failed");
        }

        // Reads exactly one frame, or returns false if the mic stalled.
        auto readFrame = [&](void) -> bool {
            size_t got = 0;
            while (got < frame) {
                size_t n = audioInRead(raw + got, frame - got, 200);
                if (n == 0) {
                    return false;
                }
                got += n;
            }
            return true;
        };

        const uint32_t SWEEP_MIN  = 0;
        const uint32_t SWEEP_MAX  = 600;    // covers both DMA queues with margin
        const uint32_t SWEEP_STEP = 20;

        int   bestDelay = -1;
        float bestErle  = -1000.0f;

        Serial.println();
        Serial.println("   delay_ms   erle_dB   mic_rms   out_rms");
        Serial.println("   --------   -------   -------   -------");

        for (uint32_t d = SWEEP_MIN; d <= SWEEP_MAX; d += SWEEP_STEP) {
            aecSetRefDelayMs(d);
            aecResetErle();

            // Let the adaptive filter re-converge on the new alignment before
            // believing anything it reports. Measuring immediately would score
            // the previous delay's filter state, not this one.
            AecStatus st;
            for (int i = 0; i < 60 && readFrame(); i++) {
                aecProcess(raw, clean, &st);
            }

            // Measure the canceller's own residual (st.micRms / st.outRms are
            // pre-gate levels), not the gated output — the gate is deliberately
            // shut during this test, since noise is playing and nobody is
            // talking, and scoring its silence would score nothing.
            float micAcc = 0, outAcc = 0;
            int   n      = 0;
            for (int i = 0; i < 30 && readFrame(); i++) {
                aecProcess(raw, clean, &st);
                micAcc += st.micRms;
                outAcc += st.outRms;
                n++;
            }
            if (n == 0) {
                continue;
            }

            const float erle = aecErleDb();
            Serial.printf("   %8u   %7.1f   %7.0f   %7.0f%s\n",
                          (unsigned)d, erle, micAcc / n, outAcc / n,
                          (erle > bestErle) ? "   <-- best so far" : "");
            if (erle > bestErle) {
                bestErle  = erle;
                bestDelay = (int)d;
            }
        }

        Serial.println();
        Serial.println("=================================================");
        Serial.printf ("  BEST: AEC_REF_DELAY_MS = %d   (erle %.1f dB)\n", bestDelay, bestErle);
        if (bestErle >= 20.0f) {
            Serial.println("  Healthy. Put that value in config.h, set BRINGUP_NONE.");
            Serial.println("  You may then raise VAD_START_SENSITIVITY to HIGH.");
        } else if (bestErle >= 10.0f) {
            Serial.println("  Marginal. Usable, but keep START_SENSITIVITY_LOW.");
            Serial.println("  Try AEC_MODE_FD_HIGH_PERF, or move the speaker off the mic.");
        } else {
            Serial.println("  TOO LOW — full duplex will not work with this.");
            Serial.println("  The speaker is probably coupling into the mic mechanically:");
            Serial.println("  decouple them, drop the volume, or set FULL_DUPLEX 0.");
        }
        Serial.println("=================================================");
        for (;;) {
            delay(1000);
        }
    }
#endif

    if (!appStateBegin()) {
        halt("PSRAM buffer allocation failed — set Tools > PSRAM to \"OPI PSRAM\"");
    }

    wifiBegin();          // non-fatal: netTask waits and wifiEnsure() retries

    // Started right after the link, not on demand: the first SNTP answer takes a
    // second or two, and the model's system prompt wants a real time by the time
    // netTask opens the session.
    timeSyncBegin();

    // Bounded: netTask opens the session moments after this returns, and the
    // system prompt can only carry a date that already exists by then.
    timeWaitSync(NTP_BOOT_WAIT_MS);

    if (!geminiBegin()) {
        halt("Gemini client init failed");
    }

    // Non-fatal: a device that cannot play music is still a working
    // assistant, and musicRequest() already answers "unavailable" when the
    // task is not running.
    if (!musicBegin()) {
        Serial.println("[BOOT] music unavailable — songs will not play");
    }

    // Non-fatal, same reasoning as musicBegin(): a device that cannot check
    // the news is still a working assistant.
    if (!newsBegin()) {
        Serial.println("[BOOT] news unavailable — lookups will not run");
    }

    if (!holidayBegin()) {
        Serial.println("[BOOT] holiday lookup unavailable");
    }

    // Audio tasks on core 1 (with the Arduino loop); the network task on core 0
    // alongside the WiFi/lwIP stacks, so TLS jitter stays off the audio core.
    xTaskCreatePinnedToCore(spkTask, "spk", 4096, NULL, 6, &hSpk, 1);
    // 6 KB rather than 4: micTask now runs the canceller and logs floats, which
    // took the high-water mark down to ~1.9 KB free.
    xTaskCreatePinnedToCore(micTask, "mic", 6144, NULL, 5, &hMic, 1);
    // 16 KB rather than 12: netTask now also runs camera init/capture, which is
    // the deepest thing on this stack. Watch the net= figure in [STAT].
    xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 4, &hNet, 0);

    memLog("boot complete");
    Serial.println("[BOOT] tasks started");
    oledLog("tasks started");
}

void loop() {
    static uint32_t lastTelemetry = 0;

    wifiEnsure();
    oledSetNet(wifiIsConnected());   // no-op unless the state actually changed

    const uint8_t st = gConv.load();
    ledSet(st == CONV_LISTENING || st == CONV_SPEAKING);

    if (LOG_TELEMETRY && millis() - lastTelemetry > 10000) {
        lastTelemetry = millis();
        Serial.printf(
#if FULL_DUPLEX
            "[STAT] %-10s erle=%.0fdB gated=%u wifi=%s rssi=%ddBm heap=%u/%u min=%u psram=%u "
            "| stack mic=%u spk=%u net=%u | drops=%u under=%u ovf=%u recon=%u muted=%u amp=%s\n",
            convStateName(st),
            aecErleDb(),
            (unsigned)aecGatedFrames(),
#else
            "[STAT] %-10s wifi=%s rssi=%ddBm heap=%u/%u min=%u psram=%u | stack mic=%u spk=%u net=%u "
            "| drops=%u under=%u ovf=%u recon=%u amp=%s\n",
            convStateName(st),
#endif
            wifiIsConnected() ? "up" : "DOWN",
            wifiRssi(),
            (unsigned)ESP.getFreeHeap(),
            (unsigned)memLargestInternal(),   // free/LARGEST: fragmentation shows here
            (unsigned)ESP.getMinFreeHeap(),
            (unsigned)ESP.getFreePsram(),
            hMic ? (unsigned)uxTaskGetStackHighWaterMark(hMic) : 0u,
            hSpk ? (unsigned)uxTaskGetStackHighWaterMark(hSpk) : 0u,
            hNet ? (unsigned)uxTaskGetStackHighWaterMark(hNet) : 0u,
            (unsigned)gDroppedChunks.load(),
            (unsigned)gUnderruns.load(),
            (unsigned)gOverflows.load(),
            (unsigned)gReconnects.load()
#if FULL_DUPLEX
            , (unsigned)sMutedChunks.load()
#endif
            , audioOutAmpIsOn() ? "on" : "off"
            );

        // Condensed vs. the Serial line above — the OLED is 21 chars wide,
        // the full [STAT] line is ~150.
        oledLog("%s %s %ddBm h=%uK",
                convStateName(st),
                wifiIsConnected() ? "wifi" : "NOWIFI",
                wifiRssi(),
                (unsigned)(ESP.getFreeHeap() / 1024));
    }

    delay(200);
}
