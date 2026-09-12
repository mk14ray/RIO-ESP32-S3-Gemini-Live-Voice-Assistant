#include <dummy.h>

#include "camera.h"
#include "memmon.h"
#include "app_state.h"
#include "b64.h"
#include "config.h"

#include <esp_camera.h>
#include <img_converters.h>
#include <esp_heap_caps.h>
#include <math.h>

// The public autofocus API arrived with the esp32-camera bundled in ESP32 Arduino
// core 3.3. Older cores have no AF entry points at all, so the whole phase
// compiles out rather than breaking the build for someone on 3.0.
#if __has_include(<esp_camera_af.h>)
  #include <esp_camera_af.h>
  #define CAM_AF_API_PRESENT 1
#else
  #define CAM_AF_API_PRESENT 0
#endif

// =============================================================================
// Every sensor implements a different subset of sensor_t's function pointers,
// and esp32-camera leaves the rest NULL — the OV2640 has no sharpness or denoise
// control, no sensor has all of them. Calling through one of those NULLs is an
// instant StoreProhibited panic in the middle of a capture, so nothing in this
// file touches a setter directly.
//
// The return value is the sensor's, not the driver's: an implemented setter that
// rejects the value still returns non-zero, which is how sensorAccepts() below
// discovers what a given sensor will actually honour.
// =============================================================================
#define CAM_TRY(s, fn, ...) ((s)->fn ? (s)->fn((s), __VA_ARGS__) : -1)
#define CAM_SET(s, fn, ...) do { (void)CAM_TRY(s, fn, __VA_ARGS__); } while (0)

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// millis() wraps every 49 days; comparing the difference as a signed value is
// correct across the wrap, comparing the values directly is not.
static inline bool expired(uint32_t deadline) {
    return (int32_t)(millis() - deadline) >= 0;
}

// =============================================================================
// Scene metering
//
// The frame the sensor just produced is a JPEG, and the only thing we want from
// it is a histogram. Decoding it at 1/8 scale gives an 80x60 thumbnail for a VGA
// frame — a few milliseconds and 9.6 KB — which is ample for exposure decisions
// and nowhere near the cost of decoding the real frame.
// =============================================================================
#define METER_BINS    32
#define METER_HI_BIN  30   // luma >= 240: a blown highlight, unrecoverable
#define METER_LO_BIN  1    // luma <  16:  a crushed shadow, partly recoverable

// An 8x8 grid of average tile luma, carried alongside the histogram. Diffing two
// consecutive grids is a 64-subtraction motion estimate — enough to tell a
// steady hand from a moving one, which is all the stillness gate needs.
#define GRID_N        8
#define GRID_CELLS    (GRID_N * GRID_N)

struct Scene {
    float   mean;     // average luma, 0-255
    float   clipHi;   // fraction of pixels pinned at white
    float   clipLo;   // fraction of pixels pinned at black
    uint8_t p5;       // 5th percentile luma
    uint8_t p95;      // 95th percentile luma
    float   rg;       // mean R / mean G — white balance, red axis
    float   bg;       // mean B / mean G — white balance, blue axis
    uint8_t grid[GRID_CELLS];
};

// Mean absolute per-tile luma change between two frames.
static float gridMotion(const Scene* a, const Scene* b) {
    uint32_t acc = 0;
    for (int i = 0; i < GRID_CELLS; i++) {
        acc += (uint32_t)abs((int)a->grid[i] - (int)b->grid[i]);
    }
    return (float)acc / (float)GRID_CELLS;
}

// Luma at which the cumulative histogram first crosses `pct` of the frame,
// reported at the centre of the bin it lands in.
static uint8_t percentile(const uint32_t* hist, uint32_t total, float pct) {
    const uint32_t want = (uint32_t)(total * pct);
    uint32_t run = 0;
    for (int b = 0; b < METER_BINS; b++) {
        run += hist[b];
        if (run >= want) {
            return (uint8_t)(b * (256 / METER_BINS) + (256 / METER_BINS) / 2);
        }
    }
    return 255;
}

static bool meterFrame(const camera_fb_t* fb, uint8_t* buf, size_t bufLen, Scene* out) {
    if (fb->format != PIXFORMAT_JPEG) {
        return false;
    }

    const uint16_t w = fb->width / 8;
    const uint16_t h = fb->height / 8;
    const size_t   need = (size_t)w * (size_t)h * 2;
    if (w == 0 || h == 0 || need > bufLen) {
        Serial.printf("[CAM] meter buffer too small: need %u B, have %u B\n",
                      (unsigned)need, (unsigned)bufLen);
        return false;
    }

    if (!jpg2rgb565(fb->buf, fb->len, buf, JPG_SCALE_8X)) {
        return false;
    }

    uint32_t hist[METER_BINS] = {0};
    uint64_t sum = 0;
    uint64_t sumR = 0, sumG = 0, sumB = 0;
    uint32_t cellSum[GRID_CELLS] = {0};
    uint32_t cellCnt[GRID_CELLS] = {0};
    const uint32_t px = (uint32_t)w * (uint32_t)h;

    for (uint16_t row = 0; row < h; row++) {
        const uint32_t cy = (uint32_t)row * GRID_N / h;

        for (uint16_t col = 0; col < w; col++) {
            const uint32_t i = (uint32_t)row * w + col;

            // jpg2rgb565 writes big-endian RGB565 — RRRRRGGG GGGBBBBB — which
            // is what the rest of the esp32-camera pipeline assumes too.
            const uint8_t hi = buf[i * 2];
            const uint8_t lo = buf[i * 2 + 1];
            const uint32_t r = hi & 0xF8;
            const uint32_t g = ((hi & 0x07) << 5) | ((lo & 0xE0) >> 3);
            const uint32_t b = (lo & 0x1F) << 3;

            // ITU-R BT.601 luma in fixed point: 0.299/0.587/0.114 scaled by 256.
            const uint32_t lum = (r * 77 + g * 151 + b * 28) >> 8;
            hist[lum >> 3]++;
            sum  += lum;
            sumR += r;
            sumG += g;
            sumB += b;

            const uint32_t cell = cy * GRID_N + ((uint32_t)col * GRID_N / w);
            cellSum[cell] += lum;
            cellCnt[cell]++;
        }
    }

    uint32_t hiCount = 0;
    uint32_t loCount = 0;
    for (int b = METER_HI_BIN; b < METER_BINS; b++) {
        hiCount += hist[b];
    }
    for (int b = 0; b <= METER_LO_BIN; b++) {
        loCount += hist[b];
    }

    out->mean   = (float)((double)sum / (double)px);
    out->clipHi = (float)hiCount / (float)px;
    out->clipLo = (float)loCount / (float)px;
    out->p5     = percentile(hist, px, 0.05f);
    out->p95    = percentile(hist, px, 0.95f);

    // Grey-world channel ratios. Not a white balance measurement in any absolute
    // sense — a genuinely red subject moves these too — but the pipeline only
    // ever compares them against the previous frame, and what it is asking is
    // "has AWB stopped moving", not "what colour is the light".
    out->rg = (sumG > 0) ? (float)((double)sumR / (double)sumG) : 1.0f;
    out->bg = (sumG > 0) ? (float)((double)sumB / (double)sumG) : 1.0f;

    for (int i = 0; i < GRID_CELLS; i++) {
        out->grid[i] = cellCnt[i] ? (uint8_t)(cellSum[i] / cellCnt[i]) : 0;
    }
    return true;
}

// =============================================================================
// Frame acquisition
// =============================================================================

// A sensor setting takes effect at a frame boundary, so the frames already in
// the driver's queue were shot under the old one. Discard those, then return the
// first frame that actually reflects what was just asked for.
//
// Bails out the moment a grab comes back empty rather than working through the
// rest. esp_camera_fb_get() blocks for ~4 s before giving up, so a sensor that
// has stopped delivering costs 4 s here instead of 12 — the difference between a
// capture that failed and a capture that took the conversation down with it.
static camera_fb_t* grabSettled(int discard) {
    for (int i = 0; i < discard; i++) {
        camera_fb_t* stale = esp_camera_fb_get();
        if (stale == nullptr) {
            return nullptr;
        }
        esp_camera_fb_return(stale);
    }
    return esp_camera_fb_get();
}

static bool meterNext(uint8_t* buf, size_t bufLen, int discard, Scene* out) {
    camera_fb_t* fb = grabSettled(discard);
    if (fb == nullptr) {
        return false;
    }
    const bool ok = meterFrame(fb, buf, bufLen, out);
    esp_camera_fb_return(fb);
    return ok;
}

// =============================================================================
// Exposure control
//
// Two sensors, two different knobs. The OV3660/OV5640 expose a real AE bias
// (set_ae_level) that shifts the auto-exposure target and lets the sensor's own
// loop do the work. The OV2640 does not implement it — the setter is a stub that
// returns an error — and the closest equivalent is the DSP brightness offset.
//
// Both are -2..2, both move the frame the same direction, so the pipeline
// discovers once which one this sensor honours and then stops caring.
// =============================================================================
struct Tuning {
    bool aeLevelOk;    // sensor honours set_ae_level
    int  bias;         // exposure bias, -2..2
    int  toneBright;   // brightness offset asked for by the tone curve, -2..2
};

static void applyExposure(sensor_t* s, const Tuning* t) {
    if (t->aeLevelOk) {
        CAM_SET(s, set_ae_level, t->bias);
        CAM_SET(s, set_brightness, t->toneBright);
    } else {
        // One knob for both jobs, so they share it. Clamping here rather than
        // refusing means a bright-scene pull-down still happens on an OV2640,
        // just with less range than the tone curve would have liked.
        CAM_SET(s, set_brightness, clampi(t->bias + t->toneBright, -2, 2));
    }
}

// =============================================================================
// Per-sensor corrections
//
// Not every sensor comes up producing a neutral, upright frame, and the ones
// that do not are wrong in ways a vision model notices badly. This is the one
// place in the file that branches on which sensor is fitted, and it does so on
// the PID the driver read over SCCB rather than on an assumption.
//
// THE OV3660 INITIALISES VERTICALLY FLIPPED. That is not a mounting question,
// it is how the sensor's reset defaults leave it, and Espressif correct for it
// in their own CameraWebServer reference with exactly the same set_vflip(1).
// Left uncorrected, every frame reaches Gemini upside down — which costs far
// more than any amount of exposure tuning wins back, because upside-down text
// is text the model has to work to read and often simply misreads.
//
// The same sensor is also visibly oversaturated out of reset; CAM_OV3660_SATURATION
// carries Espressif's empirical correction for it.
// =============================================================================
static void applySensorQuirks(sensor_t* s) {
    // Mounting orientation from config.h is the baseline; a sensor-level defect
    // is then XORed on top, so the two corrections compose instead of one of
    // them silently cancelling the other.
    int vflip   = CAM_FLIP_VERTICAL   ? 1 : 0;
    int hmirror = CAM_MIRROR_HORIZONTAL ? 1 : 0;

    if (s->id.PID == OV3660_PID) {
        vflip = !vflip;
        CAM_SET(s, set_saturation, CAM_OV3660_SATURATION);
        Serial.printf("[CAM] OV3660: correcting vertical flip, saturation %d\n",
                      CAM_OV3660_SATURATION);
    }

    CAM_SET(s, set_vflip,   vflip);
    CAM_SET(s, set_hmirror, hmirror);
}

// =============================================================================
// Base tuning
//
// Applied once, immediately after init, before a single frame is metered. These
// are the settings that make the sensor's own automatic loops run at all — the
// defaults leave several of them off — plus the detail-preserving corrections
// that cost nothing because they happen inside the sensor DSP.
// =============================================================================
static void applyBaseTuning(sensor_t* s, Tuning* t) {
    // --- the automatic loops, explicitly on -------------------------------
    CAM_SET(s, set_whitebal,     1);   // auto white balance
    CAM_SET(s, set_awb_gain,     1);   // and let it drive the channel gains
    CAM_SET(s, set_wb_mode,      0);   // 0 = auto, not one of the fixed presets
    CAM_SET(s, set_exposure_ctrl, 1);  // auto exposure
    CAM_SET(s, set_aec2,         1);   // the DSP's second AE stage: much better
                                       // than the base loop in mixed lighting,
                                       // which is exactly the HDR case
    CAM_SET(s, set_gain_ctrl,    1);   // auto gain

    // Gain ceiling is the noise/brightness trade. Start conservative — a clean
    // frame is worth more to a vision model than a bright grainy one — and let
    // the scene classifier raise it if the room turns out to be genuinely dark.
    CAM_SET(s, set_gainceiling,  GAINCEILING_4X);

    // --- detail preservation, all free ------------------------------------
    CAM_SET(s, set_bpc,      1);   // black pixel correction: kills hot pixels
    CAM_SET(s, set_wpc,      1);   // white pixel correction: kills stuck pixels
    CAM_SET(s, set_raw_gma,  1);   // gamma curve — the single biggest win for
                                   // shadow detail, and half of the HDR story
    CAM_SET(s, set_lenc,     1);   // lens shading correction: without it the
                                   // corners are visibly darker, which reads to
                                   // a model as vignetting rather than as scene
    CAM_SET(s, set_dcw,      1);   // downsize with the DSP's proper filter
                                   // instead of dropping pixels
    CAM_SET(s, set_denoise,  1);   // mild; higher smears the fine texture that
                                   // small print is made of

    // --- neutral starting point for the tone curve ------------------------
    CAM_SET(s, set_special_effect, 0);   // no effect
    CAM_SET(s, set_colorbar,       0);   // not the test pattern
    CAM_SET(s, set_contrast,       0);
    CAM_SET(s, set_saturation,     0);
    CAM_SET(s, set_sharpness,      0);

    // Orientation and per-sensor corrections last, so they win over the neutral
    // block above rather than being flattened by it.
    applySensorQuirks(s);

    // Probe the exposure knob once, with a no-op value. A sensor that returns
    // success for ae_level 0 will honour the bracket; one that does not gets the
    // brightness fallback instead.
    t->aeLevelOk  = (CAM_TRY(s, set_ae_level, 0) == 0);
    t->bias       = 0;
    t->toneBright = 0;
}

// =============================================================================
// Scene classification and tone curve — the "auto HDR" half
// =============================================================================
enum SceneKind {
    SCENE_NORMAL,
    SCENE_HIGH_RANGE,   // clipping highlights while still holding shadows
    SCENE_DARK,         // nothing anywhere near the top of the range
    SCENE_FLAT          // everything bunched in the middle
};

static SceneKind classify(const Scene* sc) {
    const int range = (int)sc->p95 - (int)sc->p5;

    if (sc->clipHi >= CAM_HDR_HI_CLIP && range >= CAM_HDR_RANGE_MIN) {
        return SCENE_HIGH_RANGE;
    }
    if (sc->p95 <= CAM_HDR_DARK_P95) {
        return SCENE_DARK;
    }
    if (range <= CAM_HDR_FLAT_RANGE) {
        return SCENE_FLAT;
    }
    return SCENE_NORMAL;
}

static const char* sceneName(SceneKind k) {
    switch (k) {
        case SCENE_HIGH_RANGE: return "high-range";
        case SCENE_DARK:       return "dark";
        case SCENE_FLAT:       return "flat";
        default:               return "normal";
    }
}

// Reshape the sensor's transfer curve to fit the measured scene into 8 bits.
// Everything here runs in the sensor DSP on the way to the JPEG encoder, so it
// costs no CPU and no extra frames — only the decision cost of having metered.
static void applyToneCurve(sensor_t* s, SceneKind kind, Tuning* t) {
    switch (kind) {
        case SCENE_HIGH_RANGE:
            // Flatten the curve so the bright end has somewhere to go, and lift
            // the floor so the shadows the flattening leaves behind stay
            // readable. Hold the gain ceiling down: in a scene this bright the
            // shadows are dark by contrast, not by lack of light, and gaining
            // them up would only add noise.
            CAM_SET(s, set_contrast, -1);
            CAM_SET(s, set_gainceiling, GAINCEILING_4X);
            t->toneBright = 1;
            break;

        case SCENE_DARK:
            // Genuinely short of light. This is the one case where more gain is
            // the right answer, and where the noise it brings is cheaper than
            // the detail it recovers.
            CAM_SET(s, set_contrast, 0);
            CAM_SET(s, set_gainceiling, GAINCEILING_32X);
            t->toneBright = 1;
            break;

        case SCENE_FLAT:
            // A flat histogram usually means a document, a screen or a wall:
            // low-contrast subjects where the detail is real but compressed.
            // Stretching it back out is what makes small text legible.
            CAM_SET(s, set_contrast, 1);
            CAM_SET(s, set_sharpness, 1);
            t->toneBright = 0;
            break;

        default:
            t->toneBright = 0;
            break;
    }
    applyExposure(s, t);
}

// =============================================================================
// Exposure convergence
// =============================================================================

// Wait for the scene to stop changing in every way that matters. Cold-start
// convergence is the entire reason the old code threw away a fixed three frames;
// measuring it means a bright room with a steady hand costs two frames instead
// of three, and a hard one gets as many as it actually needs.
//
// Three independent conditions, all of which have to hold on the same pair of
// consecutive frames:
//
//   brightness  the sensor's AE/AGC loop has settled
//   colour      its AWB loop has too, which happens later and which watching
//               luma alone cannot see
//   stillness   the scene is not moving, so the shutter frame is unlikely to be
//               motion-blurred
//
// A permanently moving scene never satisfies the third, which is why the loop is
// bounded by both a frame count and the caller's deadline: the answer to "the
// user is walking" is to shoot anyway, not to wait forever.
static bool settleScene(uint8_t* buf, size_t bufLen, uint32_t deadline, Scene* out) {
    Scene sc   = {};
    Scene prev = {};
    bool  havePrev = false;
    int   stable   = 0;
    int   frames   = 0;
    float motion   = 0.0f;

    for (int i = 0; i < CAM_SETTLE_MAX_FRAMES; i++) {
        if (!meterNext(buf, bufLen, 0, &sc)) {
            return false;
        }
        frames++;

        bool agrees = false;
        if (havePrev) {
            motion = gridMotion(&sc, &prev);
            const float dLuma   = fabsf(sc.mean - prev.mean);
            const float dColour = fabsf(sc.rg - prev.rg) + fabsf(sc.bg - prev.bg);
            const bool  still   = !CAM_STILL_ENABLE || motion <= CAM_STILL_DELTA;

            agrees = (dLuma <= CAM_AE_STABLE_DELTA)
                  && (dColour <= CAM_AWB_STABLE_DELTA)
                  && still;
        }

        stable = agrees ? stable + 1 : 0;
        if (stable >= 2) {
            break;
        }

        prev     = sc;
        havePrev = true;

        if (expired(deadline)) {
            Serial.println("[CAM] settle: out of budget, shooting as-is");
            break;
        }
    }

    // Report motion as unmeasured rather than as 0.0 when only one frame was
    // ever seen. "motion 0.0" reads as "perfectly still" — the opposite of the
    // truth, which is that nothing was compared against anything.
    if (havePrev) {
        Serial.printf("[CAM] settled after %d frame%s: luma %.1f wb %.2f/%.2f motion %.1f%s\n",
                      frames, frames == 1 ? "" : "s", sc.mean, sc.rg, sc.bg, motion,
                      stable >= 2 ? "" : "  (NOT converged)");
    } else {
        Serial.printf("[CAM] settled after %d frame: luma %.1f wb %.2f/%.2f motion n/a"
                      "  (NOT converged — single sample)\n",
                      frames, sc.mean, sc.rg, sc.bg);
    }
    *out = sc;
    return true;
}

// The sensor's AE hits its own target, which indoors is reliably darker than
// what a vision model wants. Pull it toward ours.
static void trimExposure(sensor_t* s, uint8_t* buf, size_t bufLen,
                         uint32_t deadline, Tuning* t, Scene* sc) {
    for (int step = 0; step < CAM_AE_TRIM_STEPS; step++) {
        const float err = sc->mean - CAM_AE_TARGET_LUMA;
        if (fabsf(err) <= CAM_AE_TARGET_TOL || expired(deadline)) {
            return;
        }

        const int want = clampi(t->bias + (err < 0 ? 1 : -1), -2, 2);
        if (want == t->bias) {
            return;   // already at the end of the knob
        }
        t->bias = want;
        applyExposure(s, t);

        Scene next;
        if (!meterNext(buf, bufLen, CAM_FLUSH_FRAMES, &next)) {
            return;
        }
        *sc = next;
        Serial.printf("[CAM] AE trim bias %+d -> luma %.1f\n", t->bias, sc->mean);
    }
}

// How bad an exposure is, lower being better. Highlight clipping dominates
// because a white pixel carries no information and gamma cannot invent any;
// shadow clipping is penalised but recoverable; the midtone term only settles
// ties between two otherwise clean candidates.
static float exposureScore(const Scene* sc) {
    const float midErr = fabsf(sc->mean - CAM_AE_TARGET_LUMA) / 255.0f;
    return sc->clipHi * CAM_HDR_W_HI
         + sc->clipLo * CAM_HDR_W_LO
         + midErr     * CAM_HDR_W_MID;
}

// Shoot the bracket at the metering size, not at full resolution: the point is
// to choose an exposure, and a 640x480 frame answers that question exactly as
// well as a 1600x1200 one for a fraction of the frame time. Only the winning
// setting is ever paid for at full resolution.
static void bracketExposure(sensor_t* s, uint8_t* buf, size_t bufLen,
                            uint32_t deadline, Tuning* t, Scene* sc) {
    static const int8_t kOffsets[] = CAM_HDR_BRACKET;

    float bestScore = exposureScore(sc);
    int   bestBias  = t->bias;
    Scene bestScene = *sc;

    Serial.printf("[CAM] bracket base bias %+d score %.3f\n", bestBias, bestScore);

    for (size_t i = 0; i < sizeof(kOffsets) / sizeof(kOffsets[0]); i++) {
        if (expired(deadline)) {
            break;
        }

        const int bias = clampi(t->bias + kOffsets[i], -2, 2);
        if (bias == t->bias) {
            continue;
        }

        const int saved = t->bias;
        t->bias = bias;
        applyExposure(s, t);

        Scene cand;
        if (!meterNext(buf, bufLen, CAM_FLUSH_FRAMES, &cand)) {
            t->bias = saved;
            break;
        }
        t->bias = saved;

        const float score = exposureScore(&cand);
        Serial.printf("[CAM] bracket bias %+d: luma %.1f hi %.1f%% lo %.1f%% score %.3f\n",
                      bias, cand.mean, cand.clipHi * 100.0f, cand.clipLo * 100.0f, score);

        if (score < bestScore) {
            bestScore = score;
            bestBias  = bias;
            bestScene = cand;
        }
    }

    t->bias = bestBias;
    applyExposure(s, t);
    *sc = bestScene;
    Serial.printf("[CAM] bracket chose bias %+d\n", bestBias);
}

// =============================================================================
// Autofocus
//
// A no-op on both sensors the Sense board ships with (OV2640, OV3660): they are
// fixed-focus, with no VCM to drive, and the driver says so — one query and a
// return. On a third-party OV5640 it is the difference between a readable label
// held up to the camera and a blur.
// =============================================================================
static void runAutofocus(sensor_t* s, uint32_t deadline) {
#if CAM_AF_ENABLE && CAM_AF_API_PRESENT
    if (!esp_camera_af_is_supported(s)) {
        Serial.println("[CAM] AF: sensor is fixed-focus, skipping");
        return;
    }

    esp_camera_af_config_t cfg = {};
    cfg.mode       = ESP_CAMERA_AF_MODE_AUTO;
    cfg.step_size  = 1;
    cfg.range_min  = 0;
    cfg.range_max  = 1023;
    cfg.timeout_ms = CAM_AF_TIMEOUT_MS;

    // Downloads the AF firmware into the sensor's MCU, so it is the expensive
    // part of this phase and has to happen on every capture — the sensor was
    // powerless a moment ago and kept nothing.
    esp_err_t err = esp_camera_af_init(s, &cfg);
    if (err != ESP_OK) {
        Serial.printf("[CAM] AF init failed: 0x%x — shooting unfocused\n", err);
        return;
    }

    err = esp_camera_af_trigger(s);
    if (err != ESP_OK) {
        Serial.printf("[CAM] AF trigger failed: 0x%x\n", err);
        return;
    }

    // Bounded by whatever is left of the tuning budget as well as by the AF
    // timeout: a lens still hunting when the clock runs out is not worth the
    // silence it costs, and a half-focused frame is still a frame.
    const uint32_t now  = millis();
    const uint32_t left = expired(deadline) ? 0 : (deadline - now);
    const uint32_t wait = left < CAM_AF_TIMEOUT_MS ? left : CAM_AF_TIMEOUT_MS;

    esp_camera_af_status_t st = {};
    if (wait > 0) {
        esp_camera_af_wait(s, wait, &st);
    }
    Serial.printf("[CAM] AF %s (raw 0x%02x) in %u ms\n",
                  st.focused ? "locked" : (st.busy ? "still hunting" : "unresolved"),
                  st.raw, (unsigned)(millis() - now));
#else
    (void)s;
    (void)deadline;
  #if CAM_AF_ENABLE
    Serial.println("[CAM] AF: core has no AF API, skipping");
  #endif
#endif
}

// =============================================================================
// Driver setup
// =============================================================================
static camera_config_t buildConfig(framesize_t frameSize, int fbCount) {
    camera_config_t c = {};

    c.pin_pwdn     = CAM_PIN_PWDN;
    c.pin_reset    = CAM_PIN_RESET;
    c.pin_xclk     = CAM_PIN_XCLK;
    c.pin_sccb_sda = CAM_PIN_SIOD;
    c.pin_sccb_scl = CAM_PIN_SIOC;
    c.pin_d7       = CAM_PIN_Y9;
    c.pin_d6       = CAM_PIN_Y8;
    c.pin_d5       = CAM_PIN_Y7;
    c.pin_d4       = CAM_PIN_Y6;
    c.pin_d3       = CAM_PIN_Y5;
    c.pin_d2       = CAM_PIN_Y4;
    c.pin_d1       = CAM_PIN_Y3;
    c.pin_d0       = CAM_PIN_Y2;
    c.pin_vsync    = CAM_PIN_VSYNC;
    c.pin_href     = CAM_PIN_HREF;
    c.pin_pclk     = CAM_PIN_PCLK;

    c.xclk_freq_hz = CAM_XCLK_HZ;
    c.ledc_timer   = LEDC_TIMER_0;
    c.ledc_channel = LEDC_CHANNEL_0;

    c.pixel_format = PIXFORMAT_JPEG;   // the sensor's hardware encoder does the work
    c.frame_size   = frameSize;
    c.jpeg_quality = CAM_JPEG_QUALITY;

    // The framebuffer is sized from frame_size at init and never grows, which is
    // why init happens at the full capture size even though almost every frame
    // this pipeline takes is a small metering frame: dropping the resolution
    // afterwards is free, raising it past the initial size is not possible.
    c.fb_count    = fbCount;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode   = CAMERA_GRAB_LATEST;

    return c;
}

// Bring the sensor up at the largest size that actually WORKS, which is not the
// same question as the largest size that initialises.
//
// esp_camera_init() succeeding only means the driver allocated its buffers. On
// this board the OV3660 will happily initialise at a resolution it then cannot
// stream — the DVP link does not keep up, JPEGs arrive without their end marker,
// and every fb_get() burns a ~4 s timeout returning nothing. That is what turned
// one failed capture into a 13 s hole in the conversation.
//
// So each rung is proved with a real frame before it is accepted, and a rung
// that initialises but cannot deliver is torn down and stepped past. One probe
// frame is cheap; discovering the same thing later costs 4 s a go, and the probe
// doubles as the first flush frame anyway.
static bool cameraPowerUp() {
    static const framesize_t kLadder[] = {
        CAM_FRAME_SIZE, FRAMESIZE_XGA, FRAMESIZE_SVGA, FRAMESIZE_VGA
    };

    framesize_t prev = FRAMESIZE_INVALID;

    for (size_t i = 0; i < sizeof(kLadder) / sizeof(kLadder[0]); i++) {
        const framesize_t want = kLadder[i];

        // Keep the ladder strictly descending even if CAM_FRAME_SIZE is set
        // below one of the fallback rungs.
        if (prev != FRAMESIZE_INVALID && want >= prev) {
            continue;
        }
        prev = want;

        camera_config_t cfg = buildConfig(want, CAM_FB_COUNT);
        esp_err_t err = esp_camera_init(&cfg);
        if (err != ESP_OK) {
            Serial.printf("[CAM] init at framesize %d failed: 0x%x\n", (int)want, err);
            continue;
        }

        // Runtime equivalent of CONFIG_CAMERA_PSRAM_DMA_MODE=n. It reinitialises
        // the camera internally, so it has to happen here — before any tuning,
        // which a reinit would silently throw away.
        if (esp_camera_get_psram_mode() != (bool)CAM_PSRAM_DMA_MODE) {
            const esp_err_t perr = esp_camera_set_psram_mode((bool)CAM_PSRAM_DMA_MODE);
            Serial.printf("[CAM] PSRAM DMA mode -> %s (0x%x)\n",
                          CAM_PSRAM_DMA_MODE ? "on" : "off", perr);
        }

        camera_fb_t* probe = esp_camera_fb_get();
        if (probe != nullptr) {
            esp_camera_fb_return(probe);
            Serial.printf("[CAM] framesize %d delivering frames\n", (int)want);
            return true;
        }

        Serial.printf("[CAM] framesize %d initialised but delivers no frames "
                      "— stepping down\n", (int)want);
        esp_camera_deinit();
    }

    Serial.println("[CAM] no working camera configuration");
    return false;
}

// Report what the driver actually came up as. Nothing downstream changes frame
// size any more, so this is purely diagnostic — but it is the line that says
// whether CAM_FRAME_SIZE was honoured, clamped to the sensor's ceiling, or
// stepped down by the ladder in cameraPowerUp() because the board could not
// stream it. When a capture comes back softer than expected, this is the first
// line to read.
static void logSensorInfo(sensor_t* s) {
    const framesize_t got = s->status.framesize;

    camera_sensor_info_t* info = esp_camera_sensor_get_info(&s->id);
    if (info != nullptr) {
        Serial.printf("[CAM] %s (PID 0x%04x), max framesize %d, running at %d\n",
                      info->name, (unsigned)s->id.PID,
                      (int)info->max_size, (int)got);
    }
    if (got != CAM_FRAME_SIZE) {
        Serial.printf("[CAM] requested framesize %d, running at %d\n",
                      (int)CAM_FRAME_SIZE, (int)got);
    }
}

// =============================================================================
// Sharpness
//
// Variance of the Laplacian — the standard focus measure. It reads how much
// high-frequency detail survived into the frame, which is precisely what motion
// blur and defocus destroy, and precisely what a model needs in order to resolve
// small print. Higher is sharper. The absolute value is meaningless across
// different scenes; it is only ever compared between candidates of the same one.
//
// Decoded at 1/4 rather than 1/8: at 1/8 each output pixel is a whole 8x8 block
// averaged down, which smooths away exactly the detail being measured and makes
// a mildly blurred frame indistinguishable from a sharp one.
//
// Returns -1 when the frame could not be scored, which the caller reads as
// "take it anyway" rather than as a low score.
// =============================================================================
static float sharpnessOf(const camera_fb_t* fb, uint8_t* buf, size_t bufLen) {
    const uint16_t w = fb->width / 4;
    const uint16_t h = fb->height / 4;
    if (w < 3 || h < 3 || (size_t)w * h * 2 > bufLen) {
        return -1.0f;
    }
    if (!jpg2rgb565(fb->buf, fb->len, buf, JPG_SCALE_4X)) {
        return -1.0f;
    }

    // Collapse RGB565 to an 8-bit luma plane in place. Output index i is always
    // <= the input index 2i being read, and both walk forward, so nothing is
    // overwritten before it is consumed.
    const uint32_t px = (uint32_t)w * (uint32_t)h;
    for (uint32_t i = 0; i < px; i++) {
        const uint8_t hi = buf[i * 2];
        const uint8_t lo = buf[i * 2 + 1];
        const uint32_t r = hi & 0xF8;
        const uint32_t g = ((hi & 0x07) << 5) | ((lo & 0xE0) >> 3);
        const uint32_t b = (lo & 0x1F) << 3;
        buf[i] = (uint8_t)((r * 77 + g * 151 + b * 28) >> 8);
    }

    // Integer accumulation: the Laplacian spans +/-1020, so its square tops out
    // near 1.04e6 and 120k of them still fit a uint64 with room to spare.
    int64_t  sum   = 0;
    uint64_t sumSq = 0;
    uint32_t n     = 0;

    for (uint16_t y = 1; y + 1 < h; y++) {
        const uint8_t* row  = buf + (uint32_t)y * w;
        const uint8_t* up   = row - w;
        const uint8_t* down = row + w;

        for (uint16_t x = 1; x + 1 < w; x++) {
            const int lap = 4 * (int)row[x]
                          - (int)row[x - 1] - (int)row[x + 1]
                          - (int)up[x]      - (int)down[x];
            sum   += lap;
            sumSq += (uint64_t)((int64_t)lap * lap);
            n++;
        }
    }
    if (n == 0) {
        return -1.0f;
    }

    const double mean = (double)sum / (double)n;
    return (float)((double)sumSq / (double)n - mean * mean);
}

// =============================================================================
// Shutter
//
// The only frames shot at full resolution, and the only one of them that leaves
// the device. Takes CAM_SHOT_CANDIDATES of them and keeps the sharpest, which is
// the last line of defence against a hand that moved during the resolution
// change or the autofocus cycle.
//
// The winner is base64-encoded into gImgTxBuf as soon as it takes the lead,
// rather than holding candidate frames side by side: encoding is ~10 ms against
// a framebuffer that would otherwise have to be duplicated in PSRAM, and the
// driver only has CAM_FB_COUNT of them to lend out in the first place.
//
// If an encoded message will not fit the staging buffer the shot is retaken at a
// coarser JPEG quality rather than dropped — an unusually detailed scene should
// cost a little quality, not the whole answer — and those retakes do not count
// against the candidate budget.
// =============================================================================
static bool captureAndEncode(sensor_t* s, size_t preLen, size_t postLen,
                             const char* PRE, const char* POST, size_t* outLen) {
    // Scratch for the sharpness decode. Failing to get it is not fatal: the
    // shutter falls back to a single unscored frame, which is what it used to do.
    uint8_t* sharpBuf = (uint8_t*)heap_caps_malloc(CAM_SHARP_BUF_BYTES,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const int candidates = (sharpBuf != nullptr) ? CAM_SHOT_CANDIDATES : 1;
    if (sharpBuf == nullptr && CAM_SHOT_CANDIDATES > 1) {
        Serial.println("[CAM] no PSRAM for sharpness scoring — single shot");
    }

    const uint32_t deadline = millis() + CAM_SHOT_BUDGET_MS;
    const int maxAttempts   = candidates + 2;   // headroom for size-driven retakes

    int   quality  = CAM_JPEG_QUALITY;
    int   taken    = 0;
    float bestSharp = -2.0f;
    bool  haveShot  = false;

    for (int attempt = 0; attempt < maxAttempts && taken < candidates; attempt++) {
        // The first grab has to flush the frames still in flight from the
        // resolution change; later ones are already streaming at full size.
        camera_fb_t* fb = grabSettled(attempt == 0 ? CAM_FLUSH_FRAMES : 0);
        if (fb == nullptr) {
            Serial.println("[CAM] capture failed");
            break;
        }

        const size_t need = preLen + b64EncodedLen(fb->len) + postLen;
        if (need > IMG_TX_BYTES) {
            Serial.printf("[CAM] frame too large: %u B jpeg needs %u B > %u B buffer\n",
                          (unsigned)fb->len, (unsigned)need, (unsigned)IMG_TX_BYTES);
            esp_camera_fb_return(fb);
            quality = clampi(quality + 8, 10, 63);
            Serial.printf("[CAM] retaking at jpeg quality %d\n", quality);
            CAM_SET(s, set_quality, quality);
            continue;
        }
        taken++;

        const float sharp = (sharpBuf != nullptr)
                          ? sharpnessOf(fb, sharpBuf, CAM_SHARP_BUF_BYTES)
                          : -1.0f;

        // A frame that could not be scored still beats having nothing, but never
        // displaces one that was scored and won.
        if (sharp > bestSharp || !haveShot) {
            memcpy(gImgTxBuf, PRE, preLen);
            size_t enc = 0;
            if (b64Encode(fb->buf, fb->len, gImgTxBuf + preLen,
                          IMG_TX_BYTES - preLen, &enc)) {
                // b64Encode NUL-terminates; POST overwrites that byte.
                memcpy(gImgTxBuf + preLen + enc, POST, postLen);
                *outLen   = preLen + enc + postLen;
                bestSharp = sharp;
                haveShot  = true;
                Serial.printf("[CAM] shot %d: %ux%u q%d, %u B jpeg, sharpness %.0f  <- keeping\n",
                              taken, (unsigned)fb->width, (unsigned)fb->height,
                              quality, (unsigned)fb->len, sharp);
            } else {
                Serial.println("[CAM] base64 encode failed");
            }
        } else {
            Serial.printf("[CAM] shot %d: %u B jpeg, sharpness %.0f  <- discarded\n",
                          taken, (unsigned)fb->len, sharp);
        }

        esp_camera_fb_return(fb);

        if (expired(deadline)) {
            break;
        }
    }

    if (sharpBuf != nullptr) {
        // Same reasoning as the metering buffer: no decoded image data outlives
        // the capture that produced it.
        memset(sharpBuf, 0, CAM_SHARP_BUF_BYTES);
        heap_caps_free(sharpBuf);
    }

    if (!haveShot) {
        Serial.println("[CAM] no usable frame");
    }
    return haveShot;
}

// =============================================================================
// Public entry point
// =============================================================================
bool cameraCaptureToMessage(size_t* outLen) {
    // Sent as a clientContent turn, NOT as realtimeInput.video.
    //
    // realtimeInput.video is what the Live API documents for camera streams, and
    // the server accepts it without complaint — but gemini-3.1-flash-live-preview
    // never puts those frames into context. The symptom is not an error: RIO
    // answers as if a photo arrived and describes a scene she invented. Verified
    // against the live endpoint with a synthetic test card (a red circle, a blue
    // rectangle and a black bar reading "RIO 42"): sent as realtimeInput.video she
    // described an unrelated hi-fi front panel, and sent as the clientContent turn
    // below she read the card back correctly.
    //
    // turnComplete is true because a photo is a complete user turn — an image sent
    // with turnComplete false is accepted and then leaves the model silent for the
    // rest of the exchange, which is worse than the bug this replaces.
    static const char PRE[]  = "{\"clientContent\":{\"turns\":[{\"role\":\"user\","
                                 "\"parts\":[{\"inlineData\":{"
                                   "\"mimeType\":\"image/jpeg\",\"data\":\"";
    static const char POST[] = "\"}}]}],\"turnComplete\":true}}";
    const size_t preLen  = sizeof(PRE) - 1;
    const size_t postLen = sizeof(POST) - 1;

    if (gImgTxBuf == nullptr) {
        return false;
    }

    const uint32_t started = millis();

    // esp_camera_init() takes its DMA descriptors and line buffers from internal
    // RAM. A half-initialised sensor is a far worse outcome than declining, and
    // this can run while the music path is holding a TLS session open.
    if (!memHaveInternal(CAM_INIT_INTERNAL_NEED, "camera init")) {
        return false;
    }

    if (!cameraPowerUp()) {
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s == nullptr) {
        Serial.println("[CAM] sensor handle unavailable");
        esp_camera_deinit();
        return false;
    }

    logSensorInfo(s);

    // The tuning clock starts HERE, after the sensor is up and delivering, not
    // at the top of the function.
    //
    // Init is slow and its cost varies — on this board it runs well over a
    // second, most of it SCCB register loading and PLL settling that no budget
    // can hurry. Charging the tuning budget for it meant the budget was already
    // spent before the first metering frame arrived: the settle loop got one
    // frame, declared itself out of time, and every measurement downstream ran
    // on a single unconverged sample. Which is exactly the blind warm-up this
    // pipeline exists to replace.
    const uint32_t deadline = millis() + CAM_TUNE_BUDGET_MS;

    Tuning tune = {};
    applyBaseTuning(s, &tune);
    Serial.printf("[CAM] exposure knob: %s\n", tune.aeLevelOk ? "ae_level" : "brightness");

    // Scratch for the metering thumbnails. Allocated per capture and wiped
    // before it is released: the sensor being genuinely off between captures
    // means little if a decoded frame stays sitting in PSRAM afterwards.
    uint8_t* meter = (uint8_t*)heap_caps_malloc(CAM_METER_BUF_BYTES,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (meter == nullptr) {
        Serial.println("[CAM] no PSRAM for metering — shooting untuned");
    } else {
        // --- measure at the capture resolution -----------------------------
        // Deliberately not at a smaller one: see the note in config.h. Changing
        // frame size after init leaves the driver's DMA layout behind and the
        // sensor stops delivering frames entirely.
        //
        // No flush before this. The settle loop is already the thing that waits
        // for frames to agree with each other, so a stale frame costs it one
        // iteration and nothing else — whereas flushing costs CAM_FLUSH_FRAMES+1
        // whole frames up front, which at UXGA is most of the tuning budget
        // spent before any measuring starts. The probe frame in cameraPowerUp()
        // has already cleared the pipe anyway.
        Scene sc = {};
        if (settleScene(meter, CAM_METER_BUF_BYTES, deadline, &sc)) {
            trimExposure(s, meter, CAM_METER_BUF_BYTES, deadline, &tune, &sc);

            const SceneKind kind = classify(&sc);
            Serial.printf("[CAM] scene %s: luma %.1f p5 %u p95 %u hi %.1f%% lo %.1f%%\n",
                          sceneName(kind), sc.mean, (unsigned)sc.p5, (unsigned)sc.p95,
                          sc.clipHi * 100.0f, sc.clipLo * 100.0f);

            applyToneCurve(s, kind, &tune);

#if CAM_HDR_ENABLE
            // Only the scenes where a different exposure could actually change
            // the outcome. A normal, well-lit scene has already converged and
            // bracketing it would spend frames to confirm what it just measured.
            if (kind == SCENE_HIGH_RANGE || kind == SCENE_DARK) {
                bracketExposure(s, meter, CAM_METER_BUF_BYTES, deadline, &tune, &sc);
            }
#endif
        }

        memset(meter, 0, CAM_METER_BUF_BYTES);
        heap_caps_free(meter);
    }

    // Last, so the lens settles on the scene as it will actually be shot.
    runAutofocus(s, deadline);

    const bool ok = captureAndEncode(s, preLen, postLen, PRE, POST, outLen);

    esp_camera_deinit();
    Serial.printf("[CAM] capture pipeline took %u ms\n", (unsigned)(millis() - started));
    return ok;
}
