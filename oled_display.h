#pragma once

#include <stdbool.h>

// Animated robot eyes, live subtitles and a status log on the SSD1306 I2C OLED
// (128x64, addr 0x3C, SDA=GPIO5/D4, SCL=GPIO6/D5 — the XIAO's hardware I2C).
//
// All drawing happens on one dedicated task started by oledBegin(). Nothing
// here touches Wire from the caller's thread, so these functions are safe to
// call from micTask/spkTask/netTask/loop concurrently and none of them blocks
// on an I2C transaction — they only stamp shared state under a short mutex.
//
// Two concepts, matching the RIO conversation model:
//
//   MODE  — ongoing condition, persists until changed.
//   TOOL  — one-shot overlay, ~1.4 s, then the current mode reappears by
//           itself. A mode change during an overlay is not lost: the mode is
//           stored immediately and simply becomes visible when the overlay ends.
//
// The panel is monochrome, so the colour scheme from the colour-TFT design does
// not survive. Modes are told apart by motion and shape instead, and a 1 px
// frame around the screen marks a tool overlay in place of the mint accent.

enum OledEyeMode {
    OLED_MODE_IDLE = 0,      // breathing pupil, wandering gaze, randomised blink
    OLED_MODE_LISTENING,     // larger pupils + outward pulse arcs
    OLED_MODE_PROCESSING,    // rotating dashes + pupil scanning left-right
    OLED_MODE_SPEAKING,      // pupils replaced by a bouncing 3-bar equalizer
    OLED_MODE_COUNT
};

enum OledEyeTool {
    OLED_TOOL_IMAGE = 0,     // aperture blades spinning open
    OLED_TOOL_CAPTURE,       // full-screen flash + shutter squeeze
    OLED_TOOL_MUSIC,         // small pulsing equalizer inside the eye
    OLED_TOOL_PLAY,          // pupil morphs into a play triangle
    OLED_TOOL_COUNT
};

// Brings up the panel and starts the render task. False if the SSD1306 did not
// answer at 0x3C — callers should carry on without a display rather than halt.
bool oledBegin();

// Ongoing mode. Cheap and non-blocking; safe to call from any task.
void oledSetMode(OledEyeMode mode);
OledEyeMode oledGetMode();

// One-shot overlay. Re-triggering restarts it.
void oledTriggerTool(OledEyeTool tool);

// ---- subtitles --------------------------------------------------------------
//
// Live captions under the eyes. Gemini streams transcripts in fragments
// ("Kya", " haal", " chaal?"), so these append: the buffer word-wraps and the
// last two lines stay on screen, scrolling as more arrives. The eyes shrink and
// rise to make room, and drop back once the caption expires.
//
// IMPORTANT: Adafruit_GFX has a 5x7 ASCII font and no text-shaping engine, so
// Devanagari glyphs cannot be drawn — matras and conjuncts need shaping that the
// library does not do at any font size. Devanagari is therefore ROMANISED
// rather than dropped, so a Hindi reply reads as Hinglish on the panel and the
// caption matches what the serial log prints in script:
//
//   log     [RIO] आप किस तरह की प्रॉब्लम सॉल्व करने की कोशिश कर रहे हैं
//   caption aap kis tarah kee problam solv karane kee koshish kar rahe hain
//
// Any other script still collapses to a space. The eyes animate the same way
// either way.

// Append a fragment of RIO's reply. Switching speaker starts a fresh caption.
void oledSubtitleRio(const char* text);

// Append a fragment of the user's transcript.
void oledSubtitleUser(const char* text);

// Drop the caption immediately (new turn starting).
void oledSubtitleClear();

// ---- now playing ------------------------------------------------------------
//
// A song runs for minutes while the conversation carries on around it, so this
// is NOT a tool event: it is a persistent strip along the bottom of the panel
// with its own equalizer and a scrolling title, drawn under whatever the eyes
// are doing. A live caption outranks it — captions are transient, the strip
// comes back when they expire.

void oledMusicStart(const char* title);
void oledMusicStop();

// ---- status -----------------------------------------------------------------

// Network state. A small crossed-wifi glyph appears between the eyes while
// down, and nothing is drawn while up — so it never adds clutter in normal use.
void oledSetNet(bool up);

// Shows a line of text over everything for ~2 s, then the eyes resume. Keeps
// the 8-line scrollback of the original version, so existing call sites read
// the same way. Silently does nothing if oledBegin() failed.
void oledLog(const char* fmt, ...);

// Like oledLog(), but the text stays up permanently — for halt()/FATAL paths
// where the eyes animating on would be actively misleading.
void oledFatal(const char* fmt, ...);

// Rendered frames per second, averaged over the last second. Worth watching if
// you change OLED_FRAME_MS or the I2C clock.
float oledFps();
