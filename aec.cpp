#include "aec.h"
#include "config.h"

#include <esp_heap_caps.h>
#include <esp_aec.h>
#include <esp_ns.h>
#include <esp_vad.h>
#include <math.h>

static aec_handle_t* sAec       = nullptr;
static size_t        sFrame     = 0;

// -----------------------------------------------------------------------------
// Noise suppression and voice detection
//
// Both work on 10/20/30 ms frames, which do not divide the canceller's frame
// size, so AEC output is queued and drained in whole NS frames with the
// remainder carried. Dropping the remainder instead would puncture the stream
// every frame and wreck the very speech we are trying to clean up.
// -----------------------------------------------------------------------------
#define NS_FRAME_MS      10
#define NS_FRAME_SAMPLES (MIC_SAMPLE_RATE * NS_FRAME_MS / 1000)   // 160

static ns_handle_t  sNs  = nullptr;
static vad_handle_t sVad = nullptr;

static int16_t* sCarry     = nullptr;    // AEC output awaiting a whole NS frame
static size_t   sCarryLen  = 0;
static int16_t* sNsIn      = nullptr;
static int16_t* sNsOut     = nullptr;

// Gate state
static uint32_t sGatedFrames  = 0;
static uint32_t sPlayingUntil = 0;       // hangover deadline, millis()

// esp-sr requires aligned buffers for its SIMD paths, and forbids passing the
// caller's memory directly, so the module owns all three.
static int16_t* sMicBuf = nullptr;
static int16_t* sRefBuf = nullptr;
static int16_t* sOutBuf = nullptr;

// -----------------------------------------------------------------------------
// Reference delay line
//
// Indexed by an absolute sample count rather than a head/tail pair: the read
// position is derived from the write position and a fixed delay, so a gap in
// playback shifts nothing. When the requested span predates what the ring still
// holds, the frame reads as silence, which is the truthful answer — nothing was
// audible then.
//
// Written by spkTask, read by micTask, no lock. That is safe here only because
// the read window sits AEC_REF_DELAY_MS + one frame behind the write head while
// the ring holds 512 ms: the two never address the same region. Shrink the ring
// or grow the delay towards it and this stops being true.
//
// sRingWrite is a plain aligned uint32_t, so loads and stores of it are atomic
// on this core; it is allowed to wrap, and the masked indexing stays correct
// across the wrap because the ring is a power of two.
// -----------------------------------------------------------------------------
// 1024 ms at 16 kHz. Must be a power of two (the index is masked) and must
// comfortably exceed the largest delay BRINGUP_AEC will sweep, or the sweep
// cannot reach the value it is looking for. In PSRAM: it is 32 KB, the traffic
// is trivial next to what the canceller itself does, and internal RAM is the
// scarce resource here.
#define REF_RING_SAMPLES 16384

static int16_t* sRing      = nullptr;
static uint32_t sRingWrite = 0;          // total samples ever pushed (may wrap)
static uint32_t sDelay     = 0;          // AEC_REF_DELAY_MS in 16 kHz samples
static bool     sPrimed    = false;      // enough pushed to cover delay + frame

// Decimator state (24 kHz -> 16 kHz), carried across calls so block boundaries
// do not click.
static int16_t sH1 = 0, sH2 = 0;         // 3-tap pre-filter history
static int16_t sLastY = 0;
static float   sPhase = 0.0f;

// Wall-clock anchor for the silence gap-fill in aecPushReference.
static bool     sHavePushed = false;
static uint32_t sLastPushMs = 0;

static float sErleDb = 0.0f;

static inline void ringPush(int16_t s) {
    sRing[sRingWrite & (REF_RING_SAMPLES - 1)] = s;
    sRingWrite++;
    if (!sPrimed && sRingWrite >= sDelay + (uint32_t)sFrame) {
        sPrimed = true;
    }
}

bool aecBegin() {
    // Logged before the call because the canceller allocates without checking
    // the result: if it comes up short it faults inside its own init rather
    // than returning NULL, and these two numbers are then the only evidence of
    // why. Do not remove them.
    Serial.printf("[AEC] heap before init: internal=%u PSRAM=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // The _from_config form rather than aec_create(): same parameters, but the
    // heap is stated in the open. aec_create() hardcodes caps = 0x404
    // (SPIRAM | 8BIT) where you cannot see it, which makes an out-of-memory
    // fault inside esp-sr look like it came from nowhere.
    aec_config_t cfg = {};
    cfg.mic_num       = 1;
    cfg.ref_num       = 1;      // one playback channel
    cfg.out_num       = 1;
    cfg.filter_length = AEC_FILTER_LENGTH;
    cfg.sample_rate   = MIC_SAMPLE_RATE;
    cfg.caps          = AEC_MEM_CAPS;
    cfg.mode          = AEC_MODE;
    cfg.nlp_level     = AEC_NLP_LEVEL;

    sAec = aec_create_from_config(&cfg);
    if (sAec == nullptr) {
        Serial.println("[AEC] aec_create_from_config failed");
        return false;
    }

    sFrame = (size_t)aec_get_chunksize(sAec);
    if (sFrame == 0) {
        Serial.println("[AEC] bad chunk size");
        return false;
    }

    const size_t bytes = sFrame * sizeof(int16_t);
    sMicBuf = (int16_t*)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL);
    sRefBuf = (int16_t*)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL);
    sOutBuf = (int16_t*)heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_INTERNAL);

    sRing = (int16_t*)heap_caps_calloc(REF_RING_SAMPLES, sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM);

    if (sMicBuf == nullptr || sRefBuf == nullptr || sOutBuf == nullptr || sRing == nullptr) {
        Serial.println("[AEC] buffer allocation failed");
        return false;
    }

    // --- Noise suppression -------------------------------------------------
    sNs = ns_pro_create(NS_FRAME_MS, AEC_NS_MODE, MIC_SAMPLE_RATE);
    if (sNs == nullptr) {
        Serial.println("[AEC] ns_pro_create failed");
        return false;
    }

    // --- Local voice detection ---------------------------------------------
    // Separate from Gemini's server-side VAD and doing a different job: deciding
    // whether what survived cancellation is speech at all, or just echo and room
    // noise that must never be uploaded.
    sVad = vad_create_with_param(AEC_VAD_MODE, MIC_SAMPLE_RATE, NS_FRAME_MS,
                                 AEC_VAD_MIN_SPEECH_MS, AEC_VAD_MIN_NOISE_MS);
    if (sVad == nullptr) {
        Serial.println("[AEC] vad_create_with_param failed");
        return false;
    }

    sCarry = (int16_t*)heap_caps_malloc((sFrame + NS_FRAME_SAMPLES) * sizeof(int16_t),
                                        MALLOC_CAP_INTERNAL);
    sNsIn  = (int16_t*)heap_caps_aligned_alloc(16, NS_FRAME_SAMPLES * sizeof(int16_t),
                                              MALLOC_CAP_INTERNAL);
    sNsOut = (int16_t*)heap_caps_aligned_alloc(16, NS_FRAME_SAMPLES * sizeof(int16_t),
                                              MALLOC_CAP_INTERNAL);
    if (sCarry == nullptr || sNsIn == nullptr || sNsOut == nullptr) {
        Serial.println("[AEC] front-end buffer allocation failed");
        return false;
    }

    aecSetRefDelayMs(AEC_REF_DELAY_MS);
    if (sDelay + sFrame >= REF_RING_SAMPLES) {
        Serial.printf("[AEC] AEC_REF_DELAY_MS=%d too large for the ring\n",
                      (int)AEC_REF_DELAY_MS);
        return false;
    }

    Serial.printf("[AEC] %s, nlp=%s, frame=%u samples, ref delay=%u ms (%u samples)\n",
                  aec_get_mode_string(AEC_MODE),
                  aec_get_nlp_string(AEC_NLP_LEVEL),
                  (unsigned)sFrame, (unsigned)AEC_REF_DELAY_MS, (unsigned)sDelay);
    Serial.printf("[AEC] noise suppression mode %d, VAD mode %d, "
                  "gate margin %.1fx over predicted echo\n",
                  (int)AEC_NS_MODE, (int)AEC_VAD_MODE, (double)AEC_GATE_MARGIN);
    return true;
}

size_t aecMaxOutputSamples() {
    // Worst case: a nearly full carry plus a whole frame, rounded down to NS
    // frames — never more than the input plus one NS frame.
    return sFrame + NS_FRAME_SAMPLES;
}

uint32_t aecGatedFrames() {
    return sGatedFrames;
}

static float blockRms(const int16_t* b, size_t n) {
    if (n == 0) {
        return 0.0f;
    }
    double acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += (double)b[i] * b[i];
    }
    return sqrtf((float)(acc / n));
}

size_t aecFrameSamples() {
    return sFrame;
}

void aecPushReference(const int16_t* spk, size_t samples) {
    if (sRing == nullptr) {
        return;
    }

    // Close the gap since the last push with real silence.
    //
    // The read position is the write head minus a fixed number of samples, so
    // the ring is only a timeline if it advances in real time. It does not:
    // spkTask pushes nothing between replies, so the write head freezes. Without
    // this, the "320 ms ago" the canceller reads at the start of a reply is
    // actually the tail of the PREVIOUS reply, minutes old — and it spends the
    // opening of every single reply subtracting an echo that never existed,
    // which is exactly when it most needs to be converging.
    const uint32_t now = millis();
    if (sHavePushed) {
        const uint32_t gapMs = now - sLastPushMs;
        const uint32_t ownMs = (uint32_t)(samples * 1000 / SPK_SAMPLE_RATE);
        if (gapMs > ownMs + 20) {          // 20 ms slack for scheduling jitter
            uint32_t zeros = (gapMs - ownMs) * MIC_SAMPLE_RATE / 1000;
            if (zeros > REF_RING_SAMPLES) {
                zeros = REF_RING_SAMPLES;  // idle longer than the ring: wipe it
            }
            for (uint32_t i = 0; i < zeros; i++) {
                ringPush(0);
            }
        }
    }
    sHavePushed = true;
    sLastPushMs = now;

    // 24 kHz -> 16 kHz. A gentle [1 2 1]/4 pre-filter knocks down the content
    // above the 8 kHz output Nyquist that would otherwise alias into the
    // reference and show up as uncancellable residual.
    for (size_t i = 0; i < samples; i++) {
        const int32_t y = ((int32_t)sH2 + 2 * (int32_t)sH1 + (int32_t)spk[i]) >> 2;
        sH2 = sH1;
        sH1 = spk[i];

        // Emit every output sample that falls between the previous filtered
        // sample and this one. Step 1.5 input samples per output = 2:3.
        while (sPhase < 1.0f) {
            const int32_t s = (int32_t)(sLastY + (float)(y - sLastY) * sPhase);
            ringPush((int16_t)s);
            sPhase += 1.5f;
        }
        sPhase -= 1.0f;
        sLastY = (int16_t)y;
    }
}

void aecFlushReference() {
    if (sRing == nullptr) {
        return;
    }
    // Zero the whole ring rather than resetting the write counter: the counter
    // is the time base the read position is derived from, and rewinding it
    // would misalign every frame still in flight.
    memset(sRing, 0, REF_RING_SAMPLES * sizeof(int16_t));
    sPhase  = 0.0f;
    sLastY  = 0;
    sH1     = 0;
    sH2     = 0;
    // Not a gap to be filled — the ring was just zeroed on purpose.
    sHavePushed = false;
}

/** Copy the frame that was audible while `mic` was being captured. */
static void refFetch(int16_t* dst) {
    // Before the ring has covered a full delay span there is no history to read;
    // nothing had been played, so silence is the honest reference. A latching
    // flag rather than a comparison, so the answer stays right after
    // sRingWrite wraps.
    if (!sPrimed) {
        memset(dst, 0, sFrame * sizeof(int16_t));
        return;
    }

    const uint32_t start = sRingWrite - sDelay - (uint32_t)sFrame;
    for (size_t i = 0; i < sFrame; i++) {
        dst[i] = sRing[(start + i) & (REF_RING_SAMPLES - 1)];
    }
}

size_t aecProcess(const int16_t* mic, int16_t* out, AecStatus* status) {
    AecStatus st = {};

    if (sAec == nullptr) {
        memcpy(out, mic, sFrame * sizeof(int16_t));
        if (status) *status = st;
        return sFrame;
    }

    // --- 1. Cancel ----------------------------------------------------------
    memcpy(sMicBuf, mic, sFrame * sizeof(int16_t));
    refFetch(sRefBuf);
    aec_process(sAec, sMicBuf, sRefBuf, sOutBuf);

    const float refRms = blockRms(sRefBuf, sFrame);
    const float micRms = blockRms(sMicBuf, sFrame);
    const float resRms = blockRms(sOutBuf, sFrame);

    st.micRms = micRms;

    // ERLE is only meaningful while the speaker is actually driving the room;
    // measured against silence it just reports the noise floor.
    //
    // Deliberately NOT gated on mic > out. A wrong delay makes the canceller add
    // energy rather than remove it, and clamping that away at 0 dB would make
    // every bad delay look identical during a sweep. Let it go negative.
    if (refRms > AEC_REF_ACTIVE_RMS && resRms > 0.0f && micRms > 0.0f) {
        const float inst = 20.0f * log10f(micRms / resRms);
        sErleDb = sErleDb * 0.9f + inst * 0.1f;
    }

    // --- 2. Decide whether RIO is currently audible -------------------------
    // refRms comes from the DELAYED reference, so it is already aligned with the
    // echo rather than with what was just queued. The hangover covers the room's
    // reverberant tail, which outlives the signal that caused it.
    const uint32_t now = millis();
    if (refRms > AEC_REF_ACTIVE_RMS) {
        sPlayingUntil = now + AEC_GATE_HANGOVER_MS;
    }
    st.playing = (int32_t)(sPlayingUntil - now) > 0;

    // Predict what pure echo would look like AFTER cancellation, and require the
    // survivor to stand clear of it.
    //
    // Predicted from the MIC level, not the reference: the two live in different
    // domains — reference samples are digital playback, mic samples are whatever
    // the acoustic path and mic gain deliver — and the coupling between them is
    // unknown. ERLE is by definition the mic-to-output ratio during echo-only
    // stretches, so mic / 10^(ERLE/20) is the residual in the right units with no
    // coupling estimate needed.
    //
    // It behaves correctly at both ends. Echo only: output sits at exactly
    // mic/10^(ERLE/20), a clear factor of AEC_GATE_MARGIN below the bar, so the
    // frame is dropped. User talking over RIO: their voice survives cancellation
    // while the echo does not, so output approaches mic and clears the bar
    // easily. And as ERLE falls the bar climbs towards mic itself, which
    // cancellation can never exceed — so a broken canceller simply holds the
    // gate shut rather than leaking echo into the uplink.
    const float erle     = (sErleDb > 0.0f) ? sErleDb : 0.0f;
    const float predEcho = micRms * powf(10.0f, -erle / 20.0f);
    const float bar      = predEcho * AEC_GATE_MARGIN;

    // --- 3. Suppress noise, detect speech, gate -----------------------------
    memcpy(sCarry + sCarryLen, sOutBuf, sFrame * sizeof(int16_t));
    sCarryLen += sFrame;

    size_t produced = 0;
    size_t consumed = 0;

    while (sCarryLen - consumed >= NS_FRAME_SAMPLES) {
        memcpy(sNsIn, sCarry + consumed, NS_FRAME_SAMPLES * sizeof(int16_t));
        consumed += NS_FRAME_SAMPLES;

        ns_process(sNs, sNsIn, sNsOut);

        const bool speech = (vad_process_with_trigger(sVad, sNsOut) == VAD_SPEECH);
        st.speech = st.speech || speech;

        bool pass;
        if (!st.playing) {
            // RIO is silent: nothing to be confused by, hand everything over and
            // let Gemini's own VAD do the turn-taking.
            pass = true;
        } else {
            // RIO is talking. Believe this is the user only if it is speech AND
            // it stands clear of the echo we predict is still present.
            pass = speech && (blockRms(sNsOut, NS_FRAME_SAMPLES) > bar);
        }

        if (pass) {
            memcpy(out + produced, sNsOut, NS_FRAME_SAMPLES * sizeof(int16_t));
        } else {
            // True silence, not attenuation: the aim is to leave Gemini's VAD
            // nothing at all to trigger on.
            memset(out + produced, 0, NS_FRAME_SAMPLES * sizeof(int16_t));
            st.gated = true;
            sGatedFrames++;
        }
        produced += NS_FRAME_SAMPLES;
    }

    // Keep the tail that did not fill a whole NS frame.
    sCarryLen -= consumed;
    if (sCarryLen > 0) {
        memmove(sCarry, sCarry + consumed, sCarryLen * sizeof(int16_t));
    }

    st.outRms = blockRms(out, produced);
    if (status) {
        *status = st;
    }
    return produced;
}

void aecSetRefDelayMs(uint32_t ms) {
    uint32_t d = (uint32_t)((uint64_t)ms * MIC_SAMPLE_RATE / 1000);
    if (d + sFrame >= REF_RING_SAMPLES) {
        d = REF_RING_SAMPLES - (uint32_t)sFrame - 1;
    }
    sDelay = d;
    // The prime test is against the new delay, so re-arm it rather than leaving
    // a flag that was latched for a shorter one.
    sPrimed = false;
}

void aecResetErle() {
    sErleDb = 0.0f;
}

float aecErleDb() {
    return sErleDb;
}
