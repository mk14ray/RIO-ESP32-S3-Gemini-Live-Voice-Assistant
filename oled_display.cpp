#include "oled_display.h"

#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_random.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define OLED_SDA     5
#define OLED_SCL     6
#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C

// 400 kHz rather than the 100 kHz default. One full 1 KB frame is ~24 ms at
// 400 kHz and ~95 ms at 100 kHz — the difference between animation and a
// slideshow. Drop to 100000 if a long/unshielded I2C run starts corrupting
// frames; the eyes will simply run slower.
#define OLED_I2C_HZ  400000

#define OLED_LINES      8    // 64px / 8px glyph height at text size 1
#define OLED_LINE_CHARS 21   // 128px / 6px glyph width at text size 1

// ~20 fps. The I2C write blocks on a semaphore inside the driver rather than
// spinning, so the render task yields the core for most of each frame — but
// the peripheral is still busy ~48% of the time at this rate. Raise the period
// (lower the rate) first if anything else on this board needs the I2C bus.
#define OLED_FRAME_MS   50
#define OLED_TEXT_MS    2000        // how long oledLog() holds the screen
#define OLED_STICKY     UINT32_MAX  // oledFatal(): never expires
#define OLED_SPLASH_MS  1600        // boot wordmark

// =============================================================================
// Eye style
//
// 1 = solid lit eye with its detail cut out in black (the look of the reference
//     sprite sheet, and what an OLED does best: the panel is only "on" where
//     the eye is, so the shape itself carries the expression).
// 0 = hollow outline with white detail inside, the earlier look.
//
// This flips the ink used for everything drawn *inside* an eye. Anything drawn
// outside one — pulse arcs, the tool frame, the caption — is always white.
// =============================================================================
#define EYE_FILLED 1

#if EYE_FILLED
  #define EYE_INK  SSD1306_BLACK
#else
  #define EYE_INK  SSD1306_WHITE
#endif

// =============================================================================
// Layout — 128x64, two poses that the eyes animate between.
//
//   FULL     nothing below: big eyes, centred, using the whole panel.
//   COMPACT  caption or now-playing strip showing: eyes shrink and rise,
//            freeing the bottom third under a dotted rule.
//
// Everything inside an eye is scaled by the current eye height, so one pose
// transition rescales the whole composition rather than needing a second set
// of constants.
// =============================================================================
#define EYE_H_FULL   48
#define EYE_W_FULL   48
#define EYE_CY_FULL  32
#define EYE_CXL_FULL 32
#define EYE_CXR_FULL 96

#define EYE_H_SUB    30
#define EYE_W_SUB    40
#define EYE_CY_SUB   17
#define EYE_CXL_SUB  34
#define EYE_CXR_SUB  94

#define EYE_RADIUS   14
#define T_LAYOUT    260     // ms for the full <-> compact transition

#define STRIP_RULE_Y 37
#define SUB_COLS     21
#define SUB_BUF     192
#define SUB_HOLD_MS 4500    // caption lingers this long after the last fragment
#define SUB_Y0       42
#define SUB_Y1       53

#define MUS_TITLE_MAX  64
#define MUS_TEXT_X     22   // title starts right of the strip equalizer
#define MUS_TEXT_Y     47
#define MUS_EQ_BARS     4
#define MUS_EQ_W        3
#define MUS_EQ_GAP      2
#define MUS_EQ_CY      51
#define MUS_EQ_MIN      4
#define MUS_EQ_MAX     18
#define MUS_SCROLL_MS  60   // ms per pixel of marquee travel
#define MUS_GAP        30   // blank pixels between marquee repeats

// Sizes below are for the FULL pose and scale down with the eye.
#define PUPIL_R       9    // idle base
#define PUPIL_R_MIN   7    // breathe trough (0.85x)
#define PUPIL_R_LSTN 10    // listening
#define PUPIL_R_PROC  5    // processing
#define PUPIL_SCAN   10    // scan sweep, +/-
#define GAZE_X        6    // idle saccade range, +/-
#define GAZE_Y        4

// Pulse arcs live entirely OUTSIDE the eye box and are limited to a wedge
// around the horizontal, so they never cut across the eye's top or bottom edge.
#define WAVE_R_START 30
#define WAVE_R_END   46
#define WAVE_SPAN_DEG 44   // +/- from horizontal

// Rotating dashes live entirely INSIDE the eye. At 45 degrees a ring of radius
// r sits r*0.707 from centre on each axis, so this must stay well under the
// eye's half-height (24 in the full pose) or the dashes cross the outline.
#define PROC_RING_R  16
#define PROC_DASHES   8

#define BAR_COUNT     3
#define BAR_W         5
#define BAR_GAP       4
#define BAR_MIN       6
#define BAR_MAX      30

#define TOOL_R        9    // tool icon radius (~18px across)
#define AP_SPOKES     6
#define MUS_BAR_W     3
#define MUS_BAR_GAP   3
#define MUS_BAR_MIN   6
#define MUS_BAR_MAX  16

// ---- timings, transcribed from the HTML reference ---------------------------
#define T_BREATHE     3000
#define T_BLINK        160
#define T_BLINK_MIN   2600
#define T_BLINK_MAX   3800
#define T_WAVE        1600
#define T_WAVE_STAG    550
#define T_PROC_SPIN   1400
#define T_PROC_SCAN   1100
#define T_EQ           850
#define T_EQ_STAG      150
#define T_TOOL        1400
#define T_APERTURE    1000
#define T_FLASH        220
#define T_SHUTTER      500
#define T_MUS_EQ       600
#define T_PLAY_POP     500
#define T_GAZE_MIN     700   // how long a saccade target is held
#define T_GAZE_MAX    3000
#define T_CARET        480   // caption typing caret blink

// =============================================================================
// Shared state
//
// Every mutator takes sLock briefly to stamp this struct; the render task takes
// a copy under the same lock and then draws from the copy with the lock
// released. The 24 ms I2C write therefore never happens while the lock is held,
// so a call from spkTask or netTask waits microseconds, not a frame.
// =============================================================================
struct OledState {
    OledEyeMode mode;
    OledEyeTool tool;
    uint32_t    toolStart;
    bool        toolArmed;
    uint32_t    textUntil;
    char        lines[OLED_LINES][OLED_LINE_CHARS + 1];

    char        sub[SUB_BUF];      // current caption, plain ASCII
    uint16_t    subLen;
    uint32_t    subUntil;          // 0 = no caption
    bool        subFromRio;

    bool        musicOn;
    char        musicTitle[MUS_TITLE_MAX];

    bool        netUp;
};

static Adafruit_SSD1306  sDisplay(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
static SemaphoreHandle_t sLock  = nullptr;
static volatile bool     sReady = false;
static OledState         sState;
static uint32_t          sBootAt = 0;

static float    sFps        = 0.0f;
static uint32_t sFrameCount = 0;
static uint32_t sFpsStamp   = 0;

// =============================================================================
// Fixed-point animation helpers
//
// Everything is derived from millis(), so a dropped frame costs nothing and
// animations stay in phase regardless of the frame rate actually achieved.
// =============================================================================

/** Position within a repeating period, 0..1000. */
static int32_t phase1000(uint32_t t, uint32_t period) {
    return (int32_t)((uint64_t)(t % period) * 1000ULL / period);
}

/** Yo-yo: 0 -> 1000 -> 0 across one period. */
static int32_t pingpong1000(uint32_t t, uint32_t period) {
    const int32_t p = phase1000(t, period);
    return (p < 500) ? p * 2 : (1000 - p) * 2;
}

/** smoothstep, standing in for CSS ease-in-out. */
static int32_t smooth1000(int32_t k) {
    if (k < 0)    k = 0;
    if (k > 1000) k = 1000;
    return (k * k / 1000) * (3000 - 2 * k) / 1000;
}

static int32_t lerp1000(int32_t a, int32_t b, int32_t k) {
    return a + (b - a) * k / 1000;
}

/** Exponential-ish approach with a guaranteed final step, so integer division
    truncating toward zero cannot leave a value one short of its target. */
static int easeTo(int cur, int tgt) {
    const int d = tgt - cur;
    if (d == 0) {
        return cur;
    }
    const int step = d / 3;
    return cur + (step != 0 ? step : (d > 0 ? 1 : -1));
}

// =============================================================================
// Pose
// =============================================================================
struct Layout {
    int cy, w, h, cxL, cxR;
    int32_t sc;        // scale of everything inside the eye, 1000 = FULL pose
};

static Layout poseFor(int32_t lay) {
    const int32_t k = smooth1000(lay);
    Layout L;
    L.cy  = lerp1000(EYE_CY_FULL,  EYE_CY_SUB,  k);
    L.w   = lerp1000(EYE_W_FULL,   EYE_W_SUB,   k);
    L.h   = lerp1000(EYE_H_FULL,   EYE_H_SUB,   k);
    L.cxL = lerp1000(EYE_CXL_FULL, EYE_CXL_SUB, k);
    L.cxR = lerp1000(EYE_CXR_FULL, EYE_CXR_SUB, k);
    L.sc  = L.h * 1000 / EYE_H_FULL;
    return L;
}

static int scl(int v, int32_t sc) {
    return v * sc / 1000;
}

// =============================================================================
// Drawing
// =============================================================================

/** Arc limited to an angle wedge, with an ordered dither that thins it out as
    `fade` rises. drawCircleHelper() can only do whole quadrants, which is what
    made the pulse rings cut across the eye's top and bottom edges; this also
    gives the rings somewhere to go visually instead of vanishing in one frame,
    which is the closest a 1-bit panel gets to fading out. */
static void drawArcFaded(int cx, int cy, int r, float a0, float a1,
                         int32_t fade1000, uint16_t colour) {
    if (r <= 0) {
        return;
    }
    const int steps = (int)(fabsf(a1 - a0) * r) + 2;
    for (int i = 0; i < steps; i++) {
        // Ordered dither along the arc: as fade rises, more of the 10-pixel
        // repeat is skipped, so the ring dissolves rather than blinking off.
        if ((int32_t)(i % 10) * 100 < fade1000) {
            continue;
        }
        const float a = a0 + (a1 - a0) * (float)i / (float)(steps - 1);
        sDisplay.drawPixel(cx + (int)lroundf(r * cosf(a)),
                           cy + (int)lroundf(r * sinf(a)), colour);
    }
}

/** The eye body. Filled or hollow depending on EYE_FILLED; radius is clamped so
    Adafruit_GFX's rounded-rect maths stays valid when the compact pose shrinks
    the box. */
static void drawEyeBody(const Layout& L, int cx) {
    int r = scl(EYE_RADIUS, L.sc);
    if (r > L.h / 2) r = L.h / 2;
    if (r > L.w / 2) r = L.w / 2;
    if (r < 2)       r = 2;

#if EYE_FILLED
    sDisplay.fillRoundRect(cx - L.w / 2, L.cy - L.h / 2, L.w, L.h, r, SSD1306_WHITE);
#else
    sDisplay.drawRoundRect(cx - L.w / 2, L.cy - L.h / 2, L.w, L.h, r, SSD1306_WHITE);
#endif
}

/** Pupil, cut out of the lit eye, with a specular glint punched back into it.
    The glint is what stops the pupil reading as a hole and makes it read as a
    lens — it is the single cheapest bit of "alive" available at this size. */
static void drawPupil(const Layout& L, int cx, int r, int dx, int dy) {
    if (r <= 1) {
        return;
    }
    const int px = cx + dx;
    const int py = L.cy + dy;
    sDisplay.fillCircle(px, py, r, EYE_INK);

    // Only once the pupil is big enough that a glint is a highlight rather than
    // a bite taken out of it.
    if (r >= 5) {
        const int gr = (r >= 9) ? 2 : 1;
        sDisplay.fillCircle(px - (r * 2) / 5, py - (r * 2) / 5, gr,
                            EYE_FILLED ? SSD1306_WHITE : SSD1306_BLACK);
    }
}

/** Outward pulse arcs, always white because they live outside the eye body.
    Kept in a wedge around the horizontal and starting beyond the eye's corner
    radius, so they read as sound leaving the eye rather than crossing it. */
static void drawWave(const Layout& L, int eye, int cx, uint32_t t) {
    const float span = (float)WAVE_SPAN_DEG * (float)M_PI / 180.0f;
    // Left eye radiates left, right eye radiates right.
    const float mid  = (eye == 0) ? (float)M_PI : 0.0f;

    for (int w = 0; w < 2; w++) {
        const uint32_t off = t + (uint32_t)(1 - w) * T_WAVE_STAG;
        const int32_t  k   = phase1000(off, T_WAVE);
        const int      r   = scl(lerp1000(WAVE_R_START, WAVE_R_END, k), L.sc);
        drawArcFaded(cx, L.cy, r, mid - span, mid + span, k, SSD1306_WHITE);
    }
}

/** Rotating dashed ring, inside the eye. */
static void drawProcRing(const Layout& L, int cx, uint32_t t) {
    const float rot = (float)phase1000(t, T_PROC_SPIN) * (2.0f * (float)M_PI) / 1000.0f;
    const int   rr  = scl(PROC_RING_R, L.sc);
    for (int i = 0; i < PROC_DASHES; i++) {
        const float a = rot + (float)i * (2.0f * (float)M_PI / PROC_DASHES);
        sDisplay.fillCircle(cx + (int)lroundf(rr * cosf(a)),
                            L.cy + (int)lroundf(rr * sinf(a)), 1, EYE_INK);
    }
}

/** Speaking equalizer. Heights are pseudo-random per bar but derived from time
    rather than esp_random(), so they stay smooth across frames instead of
    jittering — the effect the colour version gets from re-aimed animations. */
static void drawBars(const Layout& L, int cx, uint32_t t) {
    const int bw  = scl(BAR_W, L.sc) < 2 ? 2 : scl(BAR_W, L.sc);
    const int gap = scl(BAR_GAP, L.sc) < 2 ? 2 : scl(BAR_GAP, L.sc);
    const int lo  = scl(BAR_MIN, L.sc) < 2 ? 2 : scl(BAR_MIN, L.sc);
    const int hi  = scl(BAR_MAX, L.sc);
    const int x0  = cx - (BAR_COUNT * bw + (BAR_COUNT - 1) * gap) / 2;

    for (int b = 0; b < BAR_COUNT; b++) {
        // Two detuned periods per bar make the loop long enough not to read as
        // one, without needing to store any per-bar state.
        const uint32_t p1 = T_EQ + (uint32_t)b * 90;
        const uint32_t p2 = T_EQ * 2 + (uint32_t)b * 170;
        const uint32_t tt = t + (uint32_t)b * T_EQ_STAG;
        const int32_t  k  = (smooth1000(pingpong1000(tt, p1)) * 2
                           + smooth1000(pingpong1000(tt, p2))) / 3;
        const int h = lerp1000(lo, hi, k);
        sDisplay.fillRect(x0 + b * (bw + gap), L.cy - h / 2, bw, h, EYE_INK);
    }
}

/** Eyelid sweeping down over the eye. A lid travelling across a stationary eye
    reads as a blink; shrinking the eye box itself reads as the eye deflating. */
static void drawLid(const Layout& L, int cx, int blink1000) {
    if (blink1000 <= 0) {
        return;
    }
    const int top = L.cy - L.h / 2;
    const int ly  = top + L.h * blink1000 / 1000;

    sDisplay.fillRect(cx - L.w / 2 - 1, top - 1, L.w + 2, ly - top + 1, SSD1306_BLACK);
    if (blink1000 > 880) {
        // Fully shut: a closed eye is a line, not an empty space.
        sDisplay.drawFastHLine(cx - L.w / 2 + 4, L.cy, L.w - 8, SSD1306_WHITE);
    }
}

// ---- tool overlays ----------------------------------------------------------
static void drawAperture(const Layout& L, int cx, uint32_t d) {
    const int32_t k    = smooth1000((int32_t)((uint64_t)d * 1000 / T_APERTURE));
    const int     rOut = scl(lerp1000(4, TOOL_R + 5, smooth1000(k * 5 / 2)), L.sc);
    const int     rIn  = rOut / 3;
    const float   rot  = (float)k * (320.0f * (float)M_PI / 180.0f) / 1000.0f;

    for (int i = 0; i < AP_SPOKES; i++) {
        const float a = rot + (float)i * (2.0f * (float)M_PI / AP_SPOKES);
        sDisplay.drawLine(cx + (int)lroundf(rIn * cosf(a)),
                          L.cy + (int)lroundf(rIn * sinf(a)),
                          cx + (int)lroundf(rOut * cosf(a)),
                          L.cy + (int)lroundf(rOut * sinf(a)),
                          EYE_INK);
    }
    if (rOut > 1) {
        sDisplay.drawCircle(cx, L.cy, rOut, EYE_INK);
    }
}

static void drawShutter(const Layout& L, int cx, uint32_t d) {
    if (d >= T_SHUTTER) {
        return;
    }
    // width 18 -> 2 -> 18, matching shutterClose's horizontal squeeze.
    const int32_t k = pingpong1000((uint32_t)d, T_SHUTTER * 2);
    const int     h = scl(TOOL_R * 2, L.sc);
    int w = lerp1000(scl(TOOL_R * 2, L.sc), 2, smooth1000(k));
    if (w < 2) w = 2;
    int r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 1)     r = 1;
    sDisplay.drawRoundRect(cx - w / 2, L.cy - h / 2, w, h, r, EYE_INK);
}

static void drawMusicIcon(const Layout& L, int cx, uint32_t t) {
    const int bw  = scl(MUS_BAR_W, L.sc) < 2 ? 2 : scl(MUS_BAR_W, L.sc);
    const int gap = scl(MUS_BAR_GAP, L.sc) < 2 ? 2 : scl(MUS_BAR_GAP, L.sc);
    const int lo  = scl(MUS_BAR_MIN, L.sc) < 2 ? 2 : scl(MUS_BAR_MIN, L.sc);
    const int hi  = scl(MUS_BAR_MAX, L.sc);
    const int x0  = cx - (BAR_COUNT * bw + (BAR_COUNT - 1) * gap) / 2;

    for (int b = 0; b < BAR_COUNT; b++) {
        const int32_t k = smooth1000(pingpong1000(t + (uint32_t)b * (T_MUS_EQ / 3), T_MUS_EQ));
        const int     h = lerp1000(lo, hi, k);
        sDisplay.fillRect(x0 + b * (bw + gap), L.cy - h / 2, bw, h, EYE_INK);
    }
}

static void drawPlay(const Layout& L, int cx, uint32_t d) {
    // scale .2 -> 1.15 -> 1, as playPop does.
    int32_t s;
    if (d < 300) {
        s = lerp1000(20, 115, smooth1000((int32_t)((uint64_t)d * 1000 / 300)));
    } else if (d < T_PLAY_POP) {
        s = lerp1000(115, 100, smooth1000((int32_t)((uint64_t)(d - 300) * 1000 / 200)));
    } else {
        s = 100;
    }

    const int w = scl((TOOL_R * 2) * (int)s / 100, L.sc);
    const int h = scl((TOOL_R * 2 + 2) * (int)s / 100, L.sc);
    if (w < 2 || h < 2) {
        return;
    }
    sDisplay.fillTriangle(cx - w / 2, L.cy - h / 2,
                          cx - w / 2, L.cy + h / 2,
                          cx + w / 2, L.cy,
                          EYE_INK);
}

// =============================================================================
// Bottom strip — captions and now-playing share it
// =============================================================================
static void drawRule() {
    // Dotted rather than solid: at 0.96" a solid 128 px line is heavy enough to
    // pull the eye away from the animation above it.
    for (int x = 0; x < OLED_WIDTH; x += 3) {
        sDisplay.drawPixel(x, STRIP_RULE_Y, SSD1306_WHITE);
    }
}

/** Wrap `s` on word boundaries and return only the final two lines — which is
    what makes the caption scroll like a subtitle track as fragments arrive,
    rather than overflowing off the bottom. */
static void subWrapTail(const char* s, char l0[SUB_COLS + 1], char l1[SUB_COLS + 1]) {
    l0[0] = '\0';
    l1[0] = '\0';

    char cur[SUB_COLS + 1];
    int  n = 0;
    const char* p = s;

    while (*p != '\0') {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        const char* w = p;
        while (*p != '\0' && *p != ' ') {
            p++;
        }
        int wl = (int)(p - w);
        if (wl > SUB_COLS) {
            wl = SUB_COLS;              // a single monster word gets truncated
        }

        const int need = (n > 0) ? wl + 1 : wl;
        if (n + need > SUB_COLS) {
            cur[n] = '\0';
            strcpy(l0, l1);
            strcpy(l1, cur);
            n = 0;
        }
        if (n > 0) {
            cur[n++] = ' ';
        }
        memcpy(cur + n, w, (size_t)wl);
        n += wl;
    }

    if (n > 0) {
        cur[n] = '\0';
        strcpy(l0, l1);
        strcpy(l1, cur);
    }
}

static void renderSubtitle(const OledState& s, uint32_t now) {
    drawRule();

    char l0[SUB_COLS + 1];
    char l1[SUB_COLS + 1];
    subWrapTail(s.sub, l0, l1);

    sDisplay.setTextSize(1);
    sDisplay.setCursor(0, SUB_Y0);
    sDisplay.print(l0);
    sDisplay.setCursor(0, SUB_Y1);
    sDisplay.print(l1);

    // Typing caret while RIO is still talking — the cue that more text is
    // coming, and the thing that makes a static caption feel live.
    if (s.subFromRio && phase1000(now, T_CARET) < 500) {
        const int cx = (int)strlen(l1) * 6;
        if (cx <= OLED_WIDTH - 5) {
            sDisplay.fillRect(cx + 1, SUB_Y1, 4, 7, SSD1306_WHITE);
        }
    }
}

/** Now-playing strip: a small equalizer plus the track title scrolling as a
    marquee. Persistent for the whole song, unlike the 1.4 s tool overlay that
    only marks the moment playback starts. */
static void renderMusicStrip(const OledState& s, uint32_t now) {
    drawRule();

    sDisplay.setTextSize(1);

    const int  avail = OLED_WIDTH - MUS_TEXT_X;
    const int  textW = (int)strlen(s.musicTitle) * 6;

    if (textW <= avail) {
        sDisplay.setCursor(MUS_TEXT_X, MUS_TEXT_Y);
        sDisplay.print(s.musicTitle);
    } else {
        // Two copies one span apart, so the tail of the title is still leaving
        // as the head re-enters and the loop has no visible seam.
        const int span = textW + MUS_GAP;
        const int off  = (int)((now / MUS_SCROLL_MS) % (uint32_t)span);
        for (int rep = 0; rep < 2; rep++) {
            sDisplay.setCursor(MUS_TEXT_X + avail - off + rep * span, MUS_TEXT_Y);
            sDisplay.print(s.musicTitle);
        }
        // The marquee is drawn full-width, then the equalizer's column is wiped
        // back to black — cheaper than clipping every glyph, and Adafruit_GFX
        // has no clip rectangle to set.
        sDisplay.fillRect(0, MUS_TEXT_Y - 1, MUS_TEXT_X - 1, 10, SSD1306_BLACK);
    }

    for (int b = 0; b < MUS_EQ_BARS; b++) {
        const uint32_t p = 520 + (uint32_t)b * 130;
        const int32_t  k = smooth1000(pingpong1000(now + (uint32_t)b * 90, p));
        const int      h = lerp1000(MUS_EQ_MIN, MUS_EQ_MAX, k);
        sDisplay.fillRect(1 + b * (MUS_EQ_W + MUS_EQ_GAP), MUS_EQ_CY - h / 2,
                          MUS_EQ_W, h, SSD1306_WHITE);
    }
}

// =============================================================================
// Chrome
// =============================================================================

/** Crossed-wifi glyph, drawn between the eyes only while the link is down. */
static void drawNetDown() {
    const int cx = 64;
    const int cy = 9;
    for (int r = 2; r <= 6; r += 2) {
        sDisplay.drawCircleHelper(cx, cy, r, 0x09, SSD1306_WHITE);   // upper half
    }
    sDisplay.drawPixel(cx, cy, SSD1306_WHITE);
    sDisplay.drawLine(cx - 6, cy - 6, cx + 6, cy + 2, SSD1306_WHITE);
}

/** Boot wordmark: "RIO" with a rule that draws itself outward from the centre. */
static void renderSplash(uint32_t since) {
    const int32_t k = smooth1000((int32_t)((uint64_t)since * 1000 / OLED_SPLASH_MS));

    sDisplay.setTextSize(3);
    sDisplay.setCursor((OLED_WIDTH - 3 * 18) / 2, 12);
    sDisplay.print(F("RIO"));

    const int half = lerp1000(0, 46, k);
    sDisplay.drawFastHLine(64 - half, 40, half * 2, SSD1306_WHITE);

    if (k > 550) {
        sDisplay.setTextSize(1);
        sDisplay.setCursor((OLED_WIDTH - 15 * 6) / 2, 50);
        sDisplay.print(F("voice assistant"));
    }
    sDisplay.setTextSize(1);
}

static void renderText(const OledState& s) {
    sDisplay.setTextSize(1);
    sDisplay.setCursor(0, 0);
    for (int i = 0; i < OLED_LINES; i++) {
        sDisplay.println(s.lines[i]);
    }
}

// =============================================================================
// Frame composition
// =============================================================================
static void renderEyes(const OledState& s, const Layout& L, uint32_t now,
                       int blink1000, int gazeX, int gazeY) {
    const bool     toolOn = s.toolArmed && (now - s.toolStart) < T_TOOL;
    const uint32_t d      = now - s.toolStart;

    // A tool overlay is marked by a frame around the screen, standing in for the
    // mint accent the colour design uses. Without it the one-shot events are
    // indistinguishable from the modes on a 1-bit panel.
    if (toolOn) {
        sDisplay.drawRect(0, 0, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);

        // The capture flash whites out the whole panel briefly. Done before
        // anything else so the rest of the frame is simply skipped.
        if (s.tool == OLED_TOOL_CAPTURE && d < (T_FLASH / 4)) {
            sDisplay.fillScreen(SSD1306_WHITE);
            return;
        }
    }

    const int cxs[2] = { L.cxL, L.cxR };

    for (int e = 0; e < 2; e++) {
        const int cx = cxs[e];

        // Pulse arcs go down first: they live outside the eye body, and drawing
        // the body over them keeps the eye's edge clean where the two meet.
        if (!toolOn && s.mode == OLED_MODE_LISTENING) {
            drawWave(L, e, cx, now);
        }

        drawEyeBody(L, cx);

        if (toolOn) {
            switch (s.tool) {
            case OLED_TOOL_IMAGE:
                drawAperture(L, cx, d);
                break;
            case OLED_TOOL_CAPTURE:
                drawShutter(L, cx, d);
                break;
            case OLED_TOOL_MUSIC:
                drawMusicIcon(L, cx, now);
                break;
            case OLED_TOOL_PLAY:
                // Pupil shrinks away as the triangle pops in — the morph.
                if (d < T_PLAY_POP / 2) {
                    const int32_t k = (int32_t)((uint64_t)d * 1000 / (T_PLAY_POP / 2));
                    drawPupil(L, cx, scl(lerp1000(PUPIL_R, 0, k), L.sc), 0, 0);
                }
                drawPlay(L, cx, d);
                break;
            default:
                break;
            }
            drawLid(L, cx, blink1000);
            continue;
        }

        switch (s.mode) {
        case OLED_MODE_IDLE:
            drawPupil(L, cx,
                      scl(lerp1000(PUPIL_R, PUPIL_R_MIN,
                                   smooth1000(pingpong1000(now, T_BREATHE))), L.sc),
                      scl(gazeX, L.sc), scl(gazeY, L.sc));
            break;

        case OLED_MODE_LISTENING:
            // Half the idle wander: attentive, not distracted.
            drawPupil(L, cx, scl(PUPIL_R_LSTN, L.sc),
                      scl(gazeX, L.sc) / 2, scl(gazeY, L.sc) / 2);
            break;

        case OLED_MODE_PROCESSING:
            drawProcRing(L, cx, now);
            drawPupil(L, cx, scl(PUPIL_R_PROC, L.sc),
                      scl(lerp1000(-PUPIL_SCAN, PUPIL_SCAN,
                                   smooth1000(pingpong1000(now, T_PROC_SCAN))), L.sc),
                      0);
            break;

        case OLED_MODE_SPEAKING:
            drawBars(L, cx, now);      // pupil replaced by the equalizer
            break;

        default:
            break;
        }

        drawLid(L, cx, blink1000);
    }

    if (!s.netUp) {
        drawNetDown();
    }
}

// =============================================================================
// Render task — the only thing in this file that touches Wire
// =============================================================================
static void oledTask(void* arg) {
    (void)arg;

    uint32_t nextBlink  = millis() + T_BLINK_MIN;
    uint32_t blinkStart = 0;
    uint32_t nextGaze   = millis() + T_GAZE_MIN;
    int      gazeX = 0, gazeY = 0, gazeTX = 0, gazeTY = 0;
    int32_t  lay   = 0;             // 0 = full pose, 1000 = compact
    OledState snap;

    for (;;) {
        const uint32_t now = millis();

        if (xSemaphoreTake(sLock, pdMS_TO_TICKS(20)) == pdTRUE) {
            snap = sState;
            xSemaphoreGive(sLock);
        }
        // On a failed take, snap still holds the previous frame's state — the
        // eyes keep animating on slightly stale data rather than stalling.

        const bool toolOn = snap.toolArmed && (now - snap.toolStart) < T_TOOL;
        const bool subOn  = snap.subUntil != 0 && now < snap.subUntil && snap.sub[0] != '\0';
        const bool musOn  = snap.musicOn && !subOn;      // a caption outranks the strip
        const bool textOn = snap.textUntil == OLED_STICKY
                         || (snap.textUntil != 0 && now < snap.textUntil);
        const bool splashOn = (now - sBootAt) < OLED_SPLASH_MS
                           && !textOn && !subOn && !toolOn && !musOn;

        // Pose eases between full and compact so the strip arriving reads as the
        // eyes making room, not as a hard cut to a different screen.
        const int32_t layTarget = (subOn || musOn) ? 1000 : 0;
        const int32_t layStep   = 1000 * OLED_FRAME_MS / T_LAYOUT;
        if (lay < layTarget) {
            lay = (lay + layStep > layTarget) ? layTarget : lay + layStep;
        } else if (lay > layTarget) {
            lay = (lay - layStep < layTarget) ? layTarget : lay - layStep;
        }

        // Blink only in the modes the reference blinks in, and never over a
        // tool overlay, where it would read as a glitch.
        if (!toolOn && (snap.mode == OLED_MODE_IDLE || snap.mode == OLED_MODE_LISTENING)) {
            if ((int32_t)(now - nextBlink) >= 0) {
                blinkStart = now;
                nextBlink  = now + T_BLINK_MIN + (esp_random() % (T_BLINK_MAX - T_BLINK_MIN));
            }
        }

        int blink1000 = 0;
        const uint32_t bd = now - blinkStart;
        if (blinkStart != 0 && bd < T_BLINK) {
            blink1000 = (bd < T_BLINK / 2) ? (int32_t)(bd * 1000 / (T_BLINK / 2))
                                           : (int32_t)((T_BLINK - bd) * 1000 / (T_BLINK / 2));
        }

        // Saccades. A pupil parked dead centre is the strongest "this is a
        // screensaver" cue there is; letting the gaze wander and settle is what
        // makes the thing look like it is actually looking at something.
        if (!toolOn && (snap.mode == OLED_MODE_IDLE || snap.mode == OLED_MODE_LISTENING)) {
            if ((int32_t)(now - nextGaze) >= 0) {
                gazeTX   = (int)(esp_random() % (2 * GAZE_X + 1)) - GAZE_X;
                gazeTY   = (int)(esp_random() % (2 * GAZE_Y + 1)) - GAZE_Y;
                nextGaze = now + T_GAZE_MIN + (esp_random() % (T_GAZE_MAX - T_GAZE_MIN));
            }
        } else {
            gazeTX = 0;
            gazeTY = 0;
        }
        gazeX = easeTo(gazeX, gazeTX);
        gazeY = easeTo(gazeY, gazeTY);

        const Layout L = poseFor(lay);

        sDisplay.clearDisplay();
        if (textOn) {
            renderText(snap);
        } else if (splashOn) {
            renderSplash(now - sBootAt);
        } else {
            renderEyes(snap, L, now, blink1000, gazeX, gazeY);
            if (subOn) {
                renderSubtitle(snap, now);
            } else if (musOn) {
                renderMusicStrip(snap, now);
            }
        }
        sDisplay.display();            // ~24 ms of I2C; the task blocks, not spins

        sFrameCount++;
        if (now - sFpsStamp >= 1000) {
            sFps        = (float)sFrameCount * 1000.0f / (float)(now - sFpsStamp);
            sFrameCount = 0;
            sFpsStamp   = now;
        }

        vTaskDelay(pdMS_TO_TICKS(OLED_FRAME_MS));
    }
}

// =============================================================================
// Text sanitising
//
// Adafruit_GFX has a 5x7 ASCII font and no text-shaping engine, so Devanagari
// glyphs cannot be drawn — matras and conjuncts need shaping the library does
// not do at any font size. RIO answers in Hindi far more often than not, so
// dropping those bytes left the caption showing only the stray Latin words in a
// reply while the serial log showed the whole sentence.
//
// Devanagari is therefore transliterated to Latin here rather than discarded:
// the caption reads "aap kis tarah kee problam solv karane kee koshish kar rahe
// hain" for the sentence the log prints in script. It is a letter-for-letter
// romanisation, not a shaping engine — which is exactly what a 21-column
// monochrome strip can carry.
// =============================================================================

enum DevClass : uint8_t {
    DV_SKIP = 0,   // accent and stress marks with nothing to say in Latin
    DV_CONS,       // consonant — carries an implicit 'a' unless a matra or virama follows
    DV_VOWEL,      // independent vowel
    DV_MATRA,      // dependent vowel sign — replaces the implicit 'a'
    DV_SIGN,       // anusvara / visarga / candrabindu — lands after the implicit 'a'
    DV_VIRAMA,     // halant — cancels the implicit 'a', which is what builds conjuncts
    DV_NUKTA,      // respells the consonant just emitted (क + ़ = क़ = "qa")
    DV_PLAIN,      // digits and danda — pass straight through
};

/** U+0900..U+097F, one entry per code point. Long vowels use the Hinglish
    spellings people actually type (ee/oo, not ii/uu) since that is what reads
    back fastest on a two-line caption. */
struct DevGlyph { char s[4]; uint8_t cls; };

static const DevGlyph kDeva[128] = {
    /* 0900 */ { "n",   DV_SIGN  }, { "n",   DV_SIGN  }, { "n",   DV_SIGN  }, { "h",  DV_SIGN  },
    /* 0904 */ { "a",   DV_VOWEL }, { "a",   DV_VOWEL }, { "aa",  DV_VOWEL }, { "i",  DV_VOWEL },
    /* 0908 */ { "ee",  DV_VOWEL }, { "u",   DV_VOWEL }, { "oo",  DV_VOWEL }, { "ri", DV_VOWEL },
    /* 090C */ { "li",  DV_VOWEL }, { "e",   DV_VOWEL }, { "e",   DV_VOWEL }, { "e",  DV_VOWEL },
    /* 0910 */ { "ai",  DV_VOWEL }, { "o",   DV_VOWEL }, { "o",   DV_VOWEL }, { "o",  DV_VOWEL },
    /* 0914 */ { "au",  DV_VOWEL }, { "k",   DV_CONS  }, { "kh",  DV_CONS  }, { "g",  DV_CONS  },
    /* 0918 */ { "gh",  DV_CONS  }, { "ng",  DV_CONS  }, { "ch",  DV_CONS  }, { "chh", DV_CONS },
    /* 091C */ { "j",   DV_CONS  }, { "jh",  DV_CONS  }, { "ny",  DV_CONS  }, { "t",  DV_CONS  },
    /* 0920 */ { "th",  DV_CONS  }, { "d",   DV_CONS  }, { "dh",  DV_CONS  }, { "n",  DV_CONS  },
    /* 0924 */ { "t",   DV_CONS  }, { "th",  DV_CONS  }, { "d",   DV_CONS  }, { "dh", DV_CONS  },
    /* 0928 */ { "n",   DV_CONS  }, { "n",   DV_CONS  }, { "p",   DV_CONS  }, { "ph", DV_CONS  },
    /* 092C */ { "b",   DV_CONS  }, { "bh",  DV_CONS  }, { "m",   DV_CONS  }, { "y",  DV_CONS  },
    /* 0930 */ { "r",   DV_CONS  }, { "r",   DV_CONS  }, { "l",   DV_CONS  }, { "l",  DV_CONS  },
    /* 0934 */ { "l",   DV_CONS  }, { "v",   DV_CONS  }, { "sh",  DV_CONS  }, { "sh", DV_CONS  },
    /* 0938 */ { "s",   DV_CONS  }, { "h",   DV_CONS  }, { "e",   DV_MATRA }, { "o",  DV_MATRA },
    /* 093C */ { "",    DV_NUKTA }, { "",    DV_SKIP  }, { "aa",  DV_MATRA }, { "i",  DV_MATRA },
    /* 0940 */ { "ee",  DV_MATRA }, { "u",   DV_MATRA }, { "oo",  DV_MATRA }, { "ri", DV_MATRA },
    /* 0944 */ { "ri",  DV_MATRA }, { "e",   DV_MATRA }, { "e",   DV_MATRA }, { "e",  DV_MATRA },
    /* 0948 */ { "ai",  DV_MATRA }, { "o",   DV_MATRA }, { "o",   DV_MATRA }, { "o",  DV_MATRA },
    /* 094C */ { "au",  DV_MATRA }, { "",    DV_VIRAMA}, { "e",   DV_MATRA }, { "aw", DV_MATRA },
    /* 0950 */ { "om",  DV_VOWEL }, { "",    DV_SKIP  }, { "",    DV_SKIP  }, { "",   DV_SKIP  },
    /* 0954 */ { "",    DV_SKIP  }, { "e",   DV_MATRA }, { "u",   DV_MATRA }, { "oo", DV_MATRA },
    /* 0958 */ { "q",   DV_CONS  }, { "kh",  DV_CONS  }, { "g",   DV_CONS  }, { "z",  DV_CONS  },
    /* 095C */ { "r",   DV_CONS  }, { "rh",  DV_CONS  }, { "f",   DV_CONS  }, { "y",  DV_CONS  },
    /* 0960 */ { "ri",  DV_VOWEL }, { "li",  DV_VOWEL }, { "li",  DV_MATRA }, { "li", DV_MATRA },
    /* 0964 */ { ".",   DV_PLAIN }, { ".",   DV_PLAIN }, { "0",   DV_PLAIN }, { "1",  DV_PLAIN },
    /* 0968 */ { "2",   DV_PLAIN }, { "3",   DV_PLAIN }, { "4",   DV_PLAIN }, { "5",  DV_PLAIN },
    /* 096C */ { "6",   DV_PLAIN }, { "7",   DV_PLAIN }, { "8",   DV_PLAIN }, { "9",  DV_PLAIN },
    /* 0970 */ { ".",   DV_PLAIN }, { ".",   DV_PLAIN }, { "a",   DV_VOWEL }, { "o",  DV_VOWEL },
    /* 0974 */ { "o",   DV_VOWEL }, { "aw",  DV_VOWEL }, { "u",   DV_VOWEL }, { "oo", DV_VOWEL },
    /* 0978 */ { "d",   DV_CONS  }, { "zh",  DV_CONS  }, { "y",   DV_CONS  }, { "g",  DV_CONS  },
    /* 097C */ { "j",   DV_CONS  }, { "",    DV_SKIP  }, { "d",   DV_CONS  }, { "b",  DV_CONS  },
};
static_assert(sizeof(kDeva) / sizeof(kDeva[0]) == 128,
              "kDeva is indexed by cp - 0x0900 and must cover U+0900..U+097F exactly");

/** A nukta fuses with the consonant before it into one letter, so it respells
    that consonant instead of adding anything of its own. Only these eight take
    one in Hindi; anything else keeps the plain form. */
static const char* nuktaForm(uint32_t cons) {
    switch (cons) {
        case 0x0915: return "q";     // क़
        case 0x0916: return "kh";    // ख़
        case 0x0917: return "g";     // ग़
        case 0x091C: return "z";     // ज़
        case 0x0921: return "r";     // ड़
        case 0x0922: return "rh";    // ढ़
        case 0x092B: return "f";     // फ़
        case 0x092F: return "y";     // य़
        default:     return nullptr;
    }
}

/** Decode one UTF-8 code point and step past it. Malformed or truncated input
    yields the lead byte, which the caller turns into a space — the pointer must
    always advance or the caller's loop would never end. */
static const char* utf8Next(const char* p, uint32_t* cp) {
    const unsigned char b0 = (unsigned char)p[0];

    uint8_t  extra;
    uint32_t v;
    if (b0 < 0x80) {
        *cp = b0;
        return p + 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        extra = 1; v = b0 & 0x1Fu;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2; v = b0 & 0x0Fu;
    } else if ((b0 & 0xF8) == 0xF0) {
        extra = 3; v = b0 & 0x07u;
    } else {
        *cp = b0;                      // stray continuation byte
        return p + 1;
    }

    for (uint8_t i = 1; i <= extra; i++) {
        const unsigned char b = (unsigned char)p[i];
        if ((b & 0xC0) != 0x80) {      // truncated sequence, or a NUL mid-way
            *cp = b0;
            return p + i;
        }
        v = (v << 6) | (b & 0x3Fu);
    }
    *cp = v;
    return p + extra + 1;
}

/** Append `s` to `out`, stopping at the cap. */
static void appendStr(char* out, uint16_t cap, uint16_t* n, const char* s) {
    while (*s != '\0' && *n + 1 < cap) {
        out[(*n)++] = *s++;
    }
}

/** Copy `in` to `out` as something Adafruit_GFX's 5x7 ASCII font can draw:
    Devanagari is romanised, ASCII passes through, and the few XML entities the
    JioSaavn catalogue returns are unescaped (titles arrive as
    `Dil Kehta Hai (From &quot;Akele Hum Akele Tum&quot;)`). Any other script
    still collapses to a single space, so Latin words either side of it do not
    fuse into one unreadable token. */
static uint16_t sanitise(const char* in, char* out, uint16_t cap) {
    uint16_t n        = 0;
    bool     pendingA = false;     // a consonant is waiting to learn whether its
                                   // implicit 'a' survives the next code point
    uint16_t consAt   = 0;         // where that consonant's letters start in `out`
    uint32_t consCp   = 0;

    while (*in != '\0' && n + 1 < cap) {
        if (*in == '&') {
            struct { const char* ent; char ch; } kEnt[] = {
                { "&quot;", '"' }, { "&apos;", '\'' }, { "&amp;",  '&' },
                { "&lt;",   '<' }, { "&gt;",   '>' }, { "&#39;",  '\'' },
            };
            bool matched = false;
            for (const auto& e : kEnt) {
                const size_t l = strlen(e.ent);
                if (strncmp(in, e.ent, l) == 0) {
                    pendingA = false;
                    consCp   = 0;
                    out[n++] = e.ch;
                    in      += l;
                    matched  = true;
                    break;
                }
            }
            if (matched) {
                continue;
            }
        }

        uint32_t cp = 0;
        in = utf8Next(in, &cp);

        if (cp >= 0x0900 && cp <= 0x097F) {
            const DevGlyph& g = kDeva[cp - 0x0900];
            switch (g.cls) {
                case DV_CONS:
                    if (pendingA) {
                        appendStr(out, cap, &n, "a");
                    }
                    consAt   = n;
                    consCp   = cp;
                    appendStr(out, cap, &n, g.s);
                    pendingA = true;
                    break;

                case DV_NUKTA: {
                    const char* alt = nuktaForm(consCp);
                    if (alt != nullptr) {
                        n = consAt;              // rewind over the plain form
                        appendStr(out, cap, &n, alt);
                    }
                    break;                       // pendingA deliberately unchanged
                }

                case DV_VIRAMA:
                    pendingA = false;            // conjunct: no vowel between them
                    break;

                case DV_MATRA:
                    pendingA = false;            // the matra IS the vowel
                    appendStr(out, cap, &n, g.s);
                    consCp   = 0;
                    break;

                case DV_VOWEL:
                case DV_SIGN:
                    if (pendingA) {
                        appendStr(out, cap, &n, "a");
                        pendingA = false;
                    }
                    appendStr(out, cap, &n, g.s);
                    consCp = 0;
                    break;

                case DV_PLAIN:
                    pendingA = false;
                    consCp   = 0;
                    appendStr(out, cap, &n, g.s);
                    break;

                default:                         // DV_SKIP
                    break;
            }
            continue;
        }

        if (cp == 0x200C || cp == 0x200D) {
            continue;                  // ZWNJ/ZWJ steer shaping; they draw nothing
        }

        char c;
        if (cp >= 32 && cp < 127) {
            c = (char)cp;
        } else if (cp == '\n' || cp == '\t' || cp >= 0x80) {
            c = ' ';                   // control, or a script this cannot romanise
        } else {
            continue;
        }

        // Anything that is not Devanagari ends the word, and a word-final
        // implicit 'a' is not pronounced in Hindi — बात is "baat", not "baata".
        // Dropping it here also stops a fragment that ends mid-word from
        // growing a vowel that the next fragment would then write across.
        pendingA = false;
        consCp   = 0;

        // Collapse runs of whitespace, including the ones the filter just made.
        // A single LEADING space survives on purpose: transcripts arrive as
        // fragments split on word boundaries ("आप", " किस", " तरह"), and eating
        // the space each one starts with is what used to fuse them into
        // "aapkistarah". Callers that want a bare string trim it themselves.
        if (c == ' ' && n > 0 && out[n - 1] == ' ') {
            continue;
        }
        out[n++] = c;
    }

    out[n] = '\0';
    return n;
}

/** Drop leading and trailing spaces in place — for the call sites that want a
    standalone string rather than a fragment of a stream. */
static void trimSpaces(char* s) {
    char* first = s;
    while (*first == ' ') {
        first++;
    }
    if (first != s) {
        memmove(s, first, strlen(first) + 1);
    }
    for (size_t l = strlen(s); l > 0 && s[l - 1] == ' '; l--) {
        s[l - 1] = '\0';
    }
}

// =============================================================================
// Public API
// =============================================================================
bool oledBegin() {
    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(OLED_I2C_HZ);

    if (!sDisplay.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        return false;
    }

    sDisplay.setTextSize(1);
    sDisplay.setTextColor(SSD1306_WHITE);
    sDisplay.setTextWrap(false);   // truncate long lines rather than eat the scrollback

    memset(&sState, 0, sizeof(sState));
    sState.mode  = OLED_MODE_IDLE;
    sState.netUp = true;           // assume up until told otherwise, so the
                                   // "no link" glyph never flashes on at boot

    sLock = xSemaphoreCreateMutex();
    if (sLock == nullptr) {
        return false;
    }

    // Priority 1 on core 1: below micTask (5) and spkTask (6), so audio always
    // preempts rendering and the eyes drop a frame rather than the mic dropping
    // samples. Core 1 rather than core 0 keeps this off netTask's TLS work.
    if (xTaskCreatePinnedToCore(oledTask, "oled", 4096, nullptr, 1, nullptr, 1) != pdPASS) {
        return false;
    }

    sBootAt   = millis();
    sFpsStamp = sBootAt;
    sReady    = true;
    return true;
}

void oledSetMode(OledEyeMode mode) {
    if (!sReady || mode >= OLED_MODE_COUNT) {
        return;
    }
    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) == pdTRUE) {
        // Stored unconditionally, including mid-overlay: when the tool event
        // expires the render task simply starts drawing this mode. That is what
        // makes "revert to whatever mode was active" fall out for free.
        sState.mode = mode;
        xSemaphoreGive(sLock);
    }
}

OledEyeMode oledGetMode() {
    return sReady ? sState.mode : OLED_MODE_IDLE;
}

void oledTriggerTool(OledEyeTool tool) {
    if (!sReady || tool >= OLED_TOOL_COUNT) {
        return;
    }
    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) == pdTRUE) {
        sState.tool      = tool;
        sState.toolStart = millis();
        sState.toolArmed = true;
        // A tool event is the interesting thing to look at, so it takes the
        // screen back from any log text still being held.
        sState.textUntil = 0;
        xSemaphoreGive(sLock);
    }
}

void oledMusicStart(const char* title) {
    if (!sReady) {
        return;
    }
    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) == pdTRUE) {
        sanitise(title != nullptr ? title : "", sState.musicTitle, MUS_TITLE_MAX);
        trimSpaces(sState.musicTitle);   // a standalone title, not a stream fragment
        sState.musicOn = true;
        xSemaphoreGive(sLock);
    }
}

void oledMusicStop() {
    if (!sReady) {
        return;
    }
    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) == pdTRUE) {
        sState.musicOn      = false;
        sState.musicTitle[0] = '\0';
        xSemaphoreGive(sLock);
    }
}

void oledSetNet(bool up) {
    if (!sReady || sState.netUp == up) {
        return;                        // called every loop; skip the lock churn
    }
    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) == pdTRUE) {
        sState.netUp = up;
        xSemaphoreGive(sLock);
    }
}

static void subtitlePush(const char* text, bool fromRio) {
    if (!sReady || text == nullptr || *text == '\0') {
        return;
    }

    // Romanisation expands: one 3-byte Devanagari code point becomes up to four
    // Latin characters ("chh" plus its implicit 'a'), so this has to be roomier
    // than the fragment it is fed or long Hindi replies would lose their tail.
    // Both callers (netTask, 16 KB) have the stack for it.
    char clean[256];
    sanitise(text, clean, sizeof(clean));

    // Nothing renderable in this fragment — a script with no romanisation here
    // leaves only the space it collapsed to. Return before the speaker check
    // below, so such a fragment cannot blank a caption still on screen.
    if (clean[strspn(clean, " ")] == '\0') {
        return;
    }

    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;                        // drop the fragment rather than stall
    }

    // Speaker change starts a fresh caption: RIO answering should not appear to
    // continue the sentence the user just said.
    if (sState.subFromRio != fromRio || sState.subUntil == 0) {
        sState.sub[0]     = '\0';
        sState.subLen     = 0;
        sState.subFromRio = fromRio;
    }

    for (const char* p = clean; *p != '\0'; p++) {
        if (*p == ' ' && (sState.subLen == 0 || sState.sub[sState.subLen - 1] == ' ')) {
            continue;
        }
        // Full: drop the oldest third. Only the last two wrapped lines are ever
        // drawn, so losing the head of a long reply costs nothing visible.
        if (sState.subLen >= SUB_BUF - 1) {
            const uint16_t keep = (SUB_BUF - 1) * 2 / 3;
            memmove(sState.sub, sState.sub + (sState.subLen - keep), keep);
            sState.subLen = keep;
        }
        sState.sub[sState.subLen++] = *p;
        sState.sub[sState.subLen]   = '\0';
    }

    sState.subUntil  = millis() + SUB_HOLD_MS;
    sState.textUntil = 0;              // a live caption outranks a stale log line
    xSemaphoreGive(sLock);
}

void oledSubtitleRio(const char* text) {
    subtitlePush(text, true);
}

void oledSubtitleUser(const char* text) {
    subtitlePush(text, false);
}

void oledSubtitleClear() {
    if (!sReady) {
        return;
    }
    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) == pdTRUE) {
        sState.sub[0]   = '\0';
        sState.subLen   = 0;
        sState.subUntil = 0;
        xSemaphoreGive(sLock);
    }
}

/** Shared by oledLog/oledFatal: scroll the buffer and set the hold deadline. */
static void pushLine(const char* msg, uint32_t until) {
    if (xSemaphoreTake(sLock, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;                        // drop the line rather than stall a caller
    }
    for (int i = 0; i < OLED_LINES - 1; i++) {
        strcpy(sState.lines[i], sState.lines[i + 1]);
    }
    strncpy(sState.lines[OLED_LINES - 1], msg, OLED_LINE_CHARS);
    sState.lines[OLED_LINES - 1][OLED_LINE_CHARS] = '\0';
    sState.textUntil = until;
    xSemaphoreGive(sLock);
}

void oledLog(const char* fmt, ...) {
    if (!sReady) {
        return;
    }
    char msg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pushLine(msg, millis() + OLED_TEXT_MS);
}

void oledFatal(const char* fmt, ...) {
    if (!sReady) {
        return;
    }
    char msg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    pushLine(msg, OLED_STICKY);
}

float oledFps() {
    return sFps;
}
