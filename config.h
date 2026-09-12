#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// RIO — standalone Gemini Live voice assistant
// Board: Seeed Studio XIAO ESP32-S3 Sense  +  MAX98357A I2S amplifier
//
// No secrets in this file. WiFi credentials and the API key live in secrets.h.
// =============================================================================

// -----------------------------------------------------------------------------
// Pin map
//
// On the ESP32-S3, PDM RX is only supported on I2S0, so the microphone is
// pinned to I2S_NUM_0 and the amplifier to I2S_NUM_1. Do not use I2S_NUM_AUTO:
// it may hand I2S1 to the mic, after which PDM cannot be allocated at all.
//
// D8/D9/D10 are also the XIAO's SPI pins, shared with the Sense board's microSD
// slot — the SD card cannot be used alongside the speaker.
// -----------------------------------------------------------------------------
#define I2S_MIC_CLK_PIN     42   // onboard PDM mic clock
#define I2S_MIC_DATA_PIN    41   // onboard PDM mic data

#define I2S_SPK_BCLK_PIN     7   // D8  -> MAX98357A BCLK
#define I2S_SPK_LRC_PIN      8   // D9  -> MAX98357A LRC (word select)
#define I2S_SPK_DIN_PIN      9   // D10 -> MAX98357A DIN

#define AMP_SD_PIN           44  // D7  -> MAX98357A SD (shutdown / channel select)

#define STATUS_LED_PIN      21   // onboard user LED (active LOW on XIAO)

// -----------------------------------------------------------------------------
// Camera (XIAO ESP32-S3 Sense, DVP parallel interface over the B2B connector)
//
// These are fixed by the Sense expansion board, not free choices. None of them
// collide with the mic (41/42), the speaker (7/8/9) or the LED (21), so the
// camera can coexist with both audio paths.
// -----------------------------------------------------------------------------
#define CAM_PIN_PWDN   -1        // not wired on this board
#define CAM_PIN_RESET  -1        // not wired on this board
#define CAM_PIN_XCLK   10
#define CAM_PIN_SIOD   40        // SCCB data  (I2C-like sensor control)
#define CAM_PIN_SIOC   39        // SCCB clock
#define CAM_PIN_Y9     48
#define CAM_PIN_Y8     11
#define CAM_PIN_Y7     12
#define CAM_PIN_Y6     14
#define CAM_PIN_Y5     16
#define CAM_PIN_Y4     18
#define CAM_PIN_Y3     17
#define CAM_PIN_Y2     15
#define CAM_PIN_VSYNC  38
#define CAM_PIN_HREF   47
#define CAM_PIN_PCLK   13

// -----------------------------------------------------------------------------
// Audio format
//
// Gemini Live consumes 16 kHz mono PCM and emits 24 kHz mono PCM. The two I2S
// controllers run at different rates independently, so no resampling is needed.
// -----------------------------------------------------------------------------
#define MIC_SAMPLE_RATE     16000
#define SPK_SAMPLE_RATE     24000

// Software gain applied to mic samples before upload.
// NOTE: 2.0f is inherited from the Groq/Whisper build. Whisper tolerated hot
// audio; Gemini's VAD does not. Too much gain makes START_SENSITIVITY_LOW
// trigger on room noise. Watch the RMS values logged by micTask and tune.
#define MIC_GAIN_FACTOR     2.0f

// Google's guidance is ~100 ms chunks. Smaller chunks multiply TLS-record and
// JSON-envelope overhead for latency that is invisible next to the 700 ms VAD
// silence window.
#define MIC_CHUNK_MS        100
#define MIC_CHUNK_SAMPLES   (MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000)   // 1600
#define MIC_CHUNK_BYTES     (MIC_CHUNK_SAMPLES * 2)                   // 3200

// -----------------------------------------------------------------------------
// Buffers (all PSRAM, all allocated once in setup())
// -----------------------------------------------------------------------------
// One outbound JSON frame: 74 B envelope + 4269 B base64 = ~4343 B. 5120 gives headroom.
#define TX_SLOT_BYTES        5120
#define TX_SLOT_COUNT        6

// Gemini streams a reply FASTER than real time, so this must hold an entire
// answer, not just a few seconds of it.
//
// MEASURED against the live API: one "explain transformers in detail" answer
// returned 1,593,122 bytes = 33.2 s of 24 kHz audio, delivered in ~20 s. An
// earlier 512 KB (10.9 s) buffer would have blocked netTask and dropped audio
// mid-sentence. 2 MB = ~43.7 s, and PSRAM is 8 MB, so the headroom is cheap.
#define PCM_STREAM_BYTES     (2 * 1024 * 1024)

// Playback jitter buffer: how much reply audio must pile up in gPcmOut before
// spkTask starts feeding the amplifier.
//
// Holding a large PCM_STREAM_BYTES is not the same as being buffered. spkTask
// used to play the first decoded byte the moment it arrived, so the buffer sat
// near empty for the whole reply and the ONLY slack absorbing network jitter
// was the I2S DMA queue: dma_desc_num * dma_frame_num = 4 * 360 frames = 60 ms.
//
// MEASURED, from the same run as above: 116 messages carrying ~286 ms of audio
// each, arriving every ~172 ms. Comfortable on average (1.66x real time) but
// only ~114 ms of slack per message, so one message late by more than ~350 ms
// drains the DMA and auto_clear emits zeros — an audible break mid-word. At the
// top of a turn there is no accumulated slack at all, which is why the first
// words broke up most often.
//
// 300 ms of pre-roll raises the tolerance to ~360 ms and rebuilds itself while
// the stream runs ahead. It costs latency once per turn: at 1.66x, 300 ms of
// audio accumulates in ~180 ms of wall clock. Raise it if the link is poor and
// breaks persist; lower it if RIO feels sluggish to start.
//
// RAISED to 500 after a live run over a phone hotspot: the [STAT] line ended at
// under=85, i.e. 85 separate mid-reply drains, each one an audible break. 500 ms
// of pre-roll takes the tolerance from ~360 ms to ~560 ms of late message. It
// costs about 120 ms more before the first word (at 1.66x, 500 ms of audio piles
// up in ~300 ms of wall clock). Put it back to 300 if RIO feels slow to start and
// under= stays near zero on your link.
#define PLAYBACK_PREBUFFER_MS       500
#define PLAYBACK_PREBUFFER_BYTES    ((size_t)SPK_SAMPLE_RATE * 2 * PLAYBACK_PREBUFFER_MS / 1000)

// Ceiling on the pre-roll wait. A reply shorter than the prebuffer, or one
// trickling in over a bad link, must not sit in the buffer unplayed waiting for
// a fill level that is never coming. (turnComplete releases it early too.)
// Must stay above the time it takes to ACCUMULATE the prebuffer, or the ceiling
// fires first and the pre-roll never actually happens: ~300 ms at 1.66x for the
// 500 ms above, so 700 leaves margin without stranding a short reply.
#define PLAYBACK_PREBUFFER_MAX_WAIT_MS  700

// -----------------------------------------------------------------------------
// Amplifier mute (MAX98357A SD on D7 / GPIO 44)
//
// SD is a level-selected pin, not a plain enable. The part reads it as:
//
//   < 0.16 V        shutdown                  driven LOW
//   0.16 - 0.77 V   (L+R)/2                   floating: the breakout's 1M-to-VIN
//                                             against the internal 100k pulldown
//                                             puts 5 V x 100k/1.1M = 0.45 V here
//   0.77 - 1.4 V    right channel only
//   > 1.4 V         left channel only         driven HIGH (3.3 V)
//
// So driving it from a GPIO moves the part from (L+R)/2 to left-only. That costs
// nothing here: audioOutWriteMono() writes the same sample into both slots, so
// L == R and (L+R)/2 == L. Output amplitude is unchanged either way.
//
// Gain is NOT set by this pin — that is the separate GAIN pad, untouched at its
// 9 dB default. Nothing here makes RIO louder.
//
// What it buys is a hardware mute, and the reason that matters is upstream of
// audio quality: a Class-D amp fed a silent I2S stream still drives its own
// noise floor into a speaker sitting centimetres from the mic, and a raised
// noise floor is exactly what holds Gemini's server-side VAD open permanently —
// the failure where transcripts keep arriving and the model never replies (see
// AEC_NS_MODE below). Shutting the amp down while RIO listens removes that
// contribution entirely. It also kills the I2S start/stop thump and drops idle
// draw to microamps, which the LiPo notices.
//
// Set to 0 to leave SD floating and disable every GPIO write to it.
// -----------------------------------------------------------------------------
#define AMP_MUTE_ENABLE       1

// Settling time after SD goes high, before the first sample is written. The part
// needs a moment to leave shutdown and writing immediately clips the front of a
// reply. Paid once per turn on the rising edge, never per chunk.
#define AMP_UNMUTE_SETTLE_MS  6

// How long the amp stays live after the last sample is handed to I2S.
//
// MUST exceed the TX DMA depth. i2s_channel_write() returns when the DMA
// *accepts* a buffer, not when the amplifier has played it, and that queue is
// dma_desc_num * dma_frame_num = 4 * 360 frames = 60 ms @ 24 kHz (audio_out.cpp).
// Muting any sooner truncates the last 60 ms of every reply. The remainder is
// margin for the room's reverberant tail. Re-measure this if you change either
// DMA figure — the same edit that moves AEC_REF_DELAY_MS moves this.
#define AMP_MUTE_LINGER_MS    250

// Reassembly buffer for one complete WebSocket message.
//
// MEASURED: largest observed server message was 41,238 bytes; even a two-word
// "Hello there." reply produced an 18,128-byte message. 96 KB gives >2x headroom
// over the observed maximum, and overflow is handled as a recoverable event
// rather than a stream desync.
//
// (For reference: 71 of 116 messages in the long-answer run exceeded 15 KB,
// which is why Links2004/arduinoWebSockets is unusable here — see ws_client.h.)
#define WS_RX_ASSEMBLY_BYTES (96 * 1024)

// Base64 decodes in independent 4-char -> 3-byte groups, so splitting the span
// on any multiple of 4 is exactly equivalent to decoding it whole.
#define B64_DECODE_CHUNK     4096                      // must be a multiple of 4
#define B64_DECODE_OUT       ((B64_DECODE_CHUNK / 4) * 3)   // 3072

// One fully-built image message. UXGA at quality 10 lands around 120-250 KB of
// JPEG on a busy scene; base64 inflates that by 4/3. 512 KB covers a ~383 KB
// frame — and the camera driver sizes its own UXGA JPEG framebuffer at about
// width*height/5 = 384 KB, so a frame large enough to overflow this is a frame
// the driver could not have delivered in the first place.
//
// If it ever happens anyway (a larger sensor, a raised CAM_FRAME_SIZE), nothing
// is truncated: camera.cpp re-shoots at a coarser JPEG quality until it fits, so
// an unusually detailed scene costs a little quality rather than the answer.
#define IMG_TX_BYTES         (512 * 1024)

// -----------------------------------------------------------------------------
// Duplex mode
//
// 1 = full duplex: the mic stays open during playback and echo cancellation
//     removes RIO's own voice, so the user can interrupt mid-reply and speech
//     can overlap. Requires working AEC — see the calibration note below.
// 0 = the original half-duplex behaviour: the mic is muted for the whole reply
//     plus SPEAK_TAIL_MS. No AEC, no barge-in, but no way to self-trigger
//     either. Keep this as the fallback if AEC cannot be tuned on your unit.
// -----------------------------------------------------------------------------
#define FULL_DUPLEX          1

// Only used when FULL_DUPLEX is 0.
#define SPEAK_TAIL_MS        300

// -----------------------------------------------------------------------------
// Echo cancellation
//
// AEC_REF_DELAY_MS IS HARDWARE-SPECIFIC AND MUST BE MEASURED WITH BRINGUP_AEC.
// Get it wrong and the canceller has nothing to correlate against: ERLE sits
// near 0 dB, RIO hears itself, its own speech comes back as [YOU] transcripts,
// and full duplex collapses into a self-interruption loop.
//
// DO NOT TRUST THE DEFAULT BELOW. It is a plausible midpoint, not a measurement,
// and arithmetic cannot settle it because the two DMA queues do not sit at the
// same occupancy:
//
//   TX (audio_out.cpp)  4 x 360 @ 24 kHz = 60 ms ceiling, and runs NEAR FULL,
//                       because i2s_channel_write blocks until the DMA accepts
//   RX (audio_in.cpp)   6 x 256 @ 16 kHz = 96 ms ceiling, but runs NEAR EMPTY,
//                       because micTask consumes as fast as frames arrive
//   acoustic flight     ~10 cm           = <1 ms
//
// So the true figure is well under the 156 ms worst case, drifts with load, and
// is only pinned down by measuring it. Run BRINGUP_AEC. Change either queue and
// measure again.
#define AEC_REF_DELAY_MS     90

// Filter span in frames. Longer covers a more reverberant room at real CPU
// cost; Espressif recommend 4.
#define AEC_FILTER_LENGTH    4

// FD = the modes tuned for full duplex rather than for feeding a recogniser.
// Start LOW_COST; move to AEC_MODE_FD_HIGH_PERF if ERLE is marginal and the
// stack/telemetry shows CPU headroom.
#define AEC_MODE             AEC_MODE_FD_LOW_COST

// Residual suppression after the linear filter. AGGR is the default and the
// right starting point; VERYAGGR buys echo suppression by chewing into the
// user's speech, which costs you the overlap that full duplex is for.
#define AEC_NLP_LEVEL        AEC_NLP_LEVEL_AGGR

// -----------------------------------------------------------------------------
// Silence suppression on the uplink
//
// While RIO is audible the gate below zeroes every frame it judges to be echo,
// and micTask used to upload that silence anyway: a 4341-byte TLS write every
// 100 ms, on the same socket the reply audio is arriving over, for a payload
// that is all zeroes. MEASURED on a live run, that stream ran unbroken through
// every reply while [STAT] counted under=85 — 85 mid-reply drains of the
// playback buffer, each one an audible break.
//
// Nothing is lost by not sending it. Gemini's VAD cannot trigger on digital
// silence, so a gap and a run of zeroes mean exactly the same thing to the
// server, and one of them leaves the link free for the reply.
//
// Two conditions, both required, and the second is the load-bearing one:
//
//   - the whole 100 ms chunk is zeroes (a chunk that carries any real audio,
//     including the first frames of a barge-in, is always sent), and
//   - RIO was driving the room during it. While RIO is QUIET every chunk goes
//     up regardless, because the server's end-of-speech timer runs on the audio
//     it actually receives — going silent during LISTENING would mean the user's
//     turn never ends.
//
// Set to 0 to go back to streaming unconditionally.
// -----------------------------------------------------------------------------
#define MIC_SKIP_GATED_UPLOAD  1

// -----------------------------------------------------------------------------
// Noise suppression
//
// Applied after cancellation. Two jobs: give Gemini cleaner speech, and stop a
// high room noise floor from holding the server-side VAD permanently open — the
// failure where transcripts keep arriving but the model never takes a turn,
// because as far as it can tell the user has not stopped speaking.
//
// 0 = mild ... 3 = most aggressive. Higher suppresses more background at the
// cost of thinning quiet speech.
// -----------------------------------------------------------------------------
#define AEC_NS_MODE          2

// -----------------------------------------------------------------------------
// Echo gate (double-talk detection)
//
// This is what makes self-hearing structurally impossible rather than merely
// unlikely. While RIO is audible, a frame is forwarded only if the local VAD
// calls it speech AND it rises above the echo level predicted from the measured
// ERLE. The bar therefore moves with how well cancellation is actually working:
//
//   good ERLE -> bar is low  -> the user can interrupt freely (true full duplex)
//   poor ERLE -> bar is high -> the mic is effectively shut while RIO speaks
//                               (degrades to half duplex, never to a loop)
//
// AEC_GATE_MARGIN is how far above the predicted echo a frame must sit to be
// believed. Raise it if RIO still occasionally hears itself; lower it if genuine
// interruptions are being swallowed. Below ~1.5 the prediction error starts to
// matter more than the signal.
#define AEC_GATE_MARGIN      2.0f

// Reference RMS above which the speaker counts as driving the room. Set just
// above the digital noise floor of the playback path.
#define AEC_REF_ACTIVE_RMS   120.0f

// How long the gate stays armed after the reference goes quiet. Covers the
// room's reverberant tail, which outlives the signal that produced it.
#define AEC_GATE_HANGOVER_MS 200

// Local VAD. Aggressive by default: its job is rejecting echo and noise, and a
// missed word costs less here than a false turn. 0 = normal ... 4 = extreme.
#define AEC_VAD_MODE           VAD_MODE_2
#define AEC_VAD_MIN_SPEECH_MS  64     // ignore blips shorter than this
#define AEC_VAD_MIN_NOISE_MS   256    // hold speech state across natural pauses

// Which heap the canceller allocates its working state from.
//
// 0x404 = SPIRAM | 8BIT, which is exactly what the convenience wrapper
// aec_create() hardcodes; we pass it explicitly through aec_create_from_config
// only so it is visible and changeable, not because the default was wrong.
//
// THE CANCELLER DOES NOT CHECK ITS OWN ALLOCATIONS. If this heap cannot satisfy
// it, heap_caps_malloc returns NULL and esp-sr stores straight through it:
//
//   Guru Meditation Error: Core 1 panic'ed (StoreProhibited)
//   EXCVADDR: 0x00000000 ... dios_ssp_aec_firfilter_init
//
// With SPIRAM caps that panic means PSRAM is missing, not that PSRAM is full —
// most often Tools > PSRAM left at "Disabled". setup() now checks for PSRAM
// before touching any of this so the cause is stated in words instead.
#define AEC_MEM_CAPS         (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

// -----------------------------------------------------------------------------
// Clock (SNTP)
//
// The ESP32 boots at the epoch and has no RTC battery, so without this every
// date RIO says is invented. The clock is only used to tell the model what time
// it is (see TIME_PROMPT_FORMAT below); nothing in the audio or camera path
// depends on wall time, so a failed sync degrades one sentence of the prompt
// rather than the device.
//
// IST-5:30 is POSIX TZ syntax, where the sign is INVERTED relative to UTC+5:30 —
// this is correct as written, and no DST rule follows it because India has none.
// -----------------------------------------------------------------------------
#define NTP_TZ           "IST-5:30"
#define NTP_SERVER_1     "pool.ntp.org"
#define NTP_SERVER_2     "time.google.com"

// Anything at or below this is the 1970 boot value or a partial sync, not a real
// date. 1735689600 = 2025-01-01T00:00:00Z, comfortably before this firmware
// existed and comfortably after anything the chip could produce on its own.
#define NTP_VALID_EPOCH  1735689600L

// How long boot waits for the first SNTP answer before starting the tasks.
//
// Without this the race is lost every cold boot: SNTP is started right after the
// link comes up, netTask opens the WebSocket a few hundred ms later, and the
// first setup message goes out with no date — MEASURED on hardware, the log read
// "clock not synced" on every run. A UDP round trip to pool.ntp.org is tens of
// milliseconds once DNS is warm, so this almost always returns in well under a
// second and only spends the full budget when NTP is genuinely unreachable.
//
// It is a bounded wait, not a requirement: on timeout the device carries on and
// the next session recycle picks the clock up.
#define NTP_BOOT_WAIT_MS  3000

// -----------------------------------------------------------------------------
// Serial logging
//
// The default build is deliberately quiet. The two switches below are what made
// the monitor unreadable: LOG_WS_FRAMES prints a line per WebSocket message,
// which is ten per second while a reply streams, and LOG_MIC_LEVEL prints every
// two seconds forever. Neither says anything a healthy run needs.
//
// What stays on is the part worth watching: transcripts, state changes, tool
// calls, errors, and the periodic [STAT] line — which is where breaks actually
// show up, as the under= counter.
//
// Turn LOG_WS_FRAMES back on when debugging the wire, and LOG_MIC_LEVEL when
// tuning MIC_GAIN_FACTOR or chasing echo (it carries rms, ERLE and the gate).
// -----------------------------------------------------------------------------
#define LOG_WS_FRAMES     0   // [WS] --> tx / <-- rx, one line per message
#define LOG_MIC_LEVEL     0   // [MIC] rms/erle/gate, every 2 s
#define LOG_TRANSCRIPTS   1   // [YOU] / [RIO]
#define LOG_TELEMETRY     1   // [STAT], every 10 s

// -----------------------------------------------------------------------------
// Gemini Live endpoint
// -----------------------------------------------------------------------------
#define GEMINI_HOST   "generativelanguage.googleapis.com"
#define GEMINI_PORT   443
#define GEMINI_PATH   "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent"
#define GEMINI_MODEL  "models/gemini-3.1-flash-live-preview"
#define GEMINI_VOICE  "Zephyr"

// Server-side VAD tuning. This is what performs turn-taking and barge-in: with
// the mic open during playback, Gemini detecting speech mid-reply is exactly
// what produces an `interrupted` and lets the user cut in.
//
// The two edges trade off against each other, and the trade is different in
// full duplex:
//   START sensitivity — how eagerly a turn begins. HIGH makes interruptions
//     feel instant, but anything AEC failed to cancel can start a turn, and the
//     assistant interrupts itself. Raise this ONLY once BRINGUP_AEC shows
//     healthy ERLE.
//   SILENCE duration — how long a pause must be before your turn is over.
//     700 ms feels sluggish in conversation; 500 ms is noticeably snappier
//     without chopping mid-sentence pauses. Lower it further only if you do not
//     mind being cut off while thinking.
#if FULL_DUPLEX
  #define VAD_START_SENSITIVITY  "START_SENSITIVITY_LOW"
  #define VAD_END_SENSITIVITY    "END_SENSITIVITY_HIGH"
  #define VAD_PREFIX_PADDING_MS  200
  #define VAD_SILENCE_DURATION_MS 500
#else
  #define VAD_START_SENSITIVITY  "START_SENSITIVITY_LOW"
  #define VAD_END_SENSITIVITY    "END_SENSITIVITY_LOW"
  #define VAD_PREFIX_PADDING_MS  300
  #define VAD_SILENCE_DURATION_MS 700
#endif

// Proactively recycle the connection before Google's ~10 min limit, in case the
// goAway notice does not arrive.
#define SESSION_RECYCLE_MS   (8UL * 60UL * 1000UL)

// -----------------------------------------------------------------------------
// Camera capture + the permission gate
// -----------------------------------------------------------------------------
// Sent as a still on demand, never streamed, so the Live API's 1 FPS video cap
// is not a constraint here. The only budget that matters is the wall-clock gap
// between "permission granted" and RIO having something to look at, and that
// gap is already dominated by the spoken permission exchange.
//
// CAM_FRAME_SIZE is a REQUEST, not a promise. camera.cpp uses whatever the
// driver actually initialised at, which is capped to the attached sensor's
// maximum — UXGA on an OV2640, QXGA on the OV3660 current Sense units ship, more
// on a third-party OV5640 — so this line does not have to be edited when the
// sensor is swapped, and over-asking cannot fail init.
//
// UXGA rather than the SVGA this used to be: 4x the pixels is the difference
// between "there is text on that box" and actually reading the text, and this
// is one still per question, not a 1 FPS stream. Quality 10 rather than 12 for
// the same reason — the extra ~40 KB buys back the JPEG ringing that eats fine
// detail like handwriting and small print.
//
// UXGA and not the OV3660's full QXGA, though, even where the sensor has it.
// Gemini re-tiles whatever it is sent into 768 px tiles, so past roughly UXGA
// the extra pixels are resampled away — what they actually buy is a 1.6x larger
// framebuffer, a 1.6x larger upload, and a longer silence before RIO answers.
// Raise it if you have a specific reason; it is not free detail.
#define CAM_FRAME_SIZE     FRAMESIZE_UXGA   // 1600x1200, capped to the sensor max
#define CAM_JPEG_QUALITY   10               // 10-63, lower is better quality

// 10 MHz, NOT the 20 MHz that worked at SVGA.
//
// The OV3660 accepts 6-27 MHz, and 20 MHz is the esp32-camera default that every
// OV2640 example uses. On this board with this sensor at high resolution it is
// too fast: frames stop completing and esp_camera_fb_get() returns nothing until
// its ~4 s timeout, which reads as "capture failed" with no other clue.
//
// Halving XCLK halves the pixel clock and roughly doubles frame time, which is
// the cost of getting a UXGA frame out at all. Once captures are reliably
// working, 20 MHz is the first thing to try raising — it directly halves the
// capture latency — but change it back the moment timeouts reappear.
#define CAM_XCLK_HZ        10000000

// PSRAM DMA mode, off.
//
// The camera driver can DMA straight into PSRAM. With the OV3660 that overflows,
// and the failure looks exactly like the XCLK one: NO-EOI / missing JPEG end
// marker, then frame timeouts. Frame buffers still live in PSRAM
// (CAMERA_FB_IN_PSRAM below) — this only controls whether the DMA engine writes
// there directly.
//
// ESP-IDF projects set this with CONFIG_CAMERA_PSRAM_DMA_MODE=n at build time.
// Arduino ships a precompiled driver, so it has to be done at runtime, and the
// runtime call reinitialises the camera — which is why camera.cpp makes it the
// very first thing after init, before any tuning that a reinit would discard.
#define CAM_PSRAM_DMA_MODE 0

// Two framebuffers, not one. The tuning pipeline below grabs a short burst of
// frames to measure the scene, and with a single buffer every grab has to wait
// out a full frame period with the sensor idle in between. The second buffer
// keeps the sensor streaming while a frame is being metered, which roughly
// halves the time the whole convergence phase takes. It costs PSRAM only while
// the camera is powered, which is only during a capture.
#define CAM_FB_COUNT       2

// Frames to discard after changing a sensor setting, before believing what the
// next frame says. Exposure, gain and resolution changes take effect at a frame
// boundary, so the frame already in flight was shot with the old values.
#define CAM_FLUSH_FRAMES   2

// -----------------------------------------------------------------------------
// Orientation
//
// These describe HOW THE CAMERA IS MOUNTED, not how the sensor behaves. Leave
// both at 0 for a board sitting the normal way up; set one if you physically
// mount the XIAO rotated or behind a mirror.
//
// Sensor defects are corrected separately and automatically — the OV3660 fitted
// to current Sense boards comes out of reset vertically flipped, and camera.cpp
// undoes that on top of whatever is set here. Do NOT set CAM_FLIP_VERTICAL to 1
// to "fix" an upside-down OV3660: that correction already happens, and setting
// this as well flips it back the wrong way round.
//
// Orientation is worth caring about. A model reading a label out of an
// upside-down frame is doing avoidable work and gets it wrong more often, and
// no amount of exposure tuning buys that back.
// -----------------------------------------------------------------------------
#define CAM_FLIP_VERTICAL     0
#define CAM_MIRROR_HORIZONTAL 0

// The OV3660 is visibly oversaturated out of reset. -2 is the correction
// Espressif ship in their own CameraWebServer reference for this exact sensor.
// It is the first thing to change if colours read wrong — the system prompt asks
// RIO to judge colours, so this one is load-bearing.
#define CAM_OV3660_SATURATION -2

// -----------------------------------------------------------------------------
// Automatic scene adaptation (auto-exposure, auto-brightness, auto HDR)
//
// The sensor is cold on every capture by design — that is the whole privacy
// story — which means its auto-exposure, auto-gain and auto-white-balance loops
// start from scratch every time and have exactly one frame's worth of scene to
// converge on. The old code paid for that with three blind warm-up frames and
// hoped. This measures instead: it meters a burst of small frames, watches the
// luma settle, then corrects what the sensor's own AE got wrong.
//
// Metering runs at the SAME resolution as the final shot, which is slower than
// it sounds like it should be and is not negotiable.
//
// The obvious optimisation — init at UXGA, drop to VGA to do the measuring,
// then go back up for the shutter — does not work, and fails in a way worth
// recording. esp_camera_init() is the only thing that ever calls cam_config(),
// which is what sizes the DMA descriptor chain and the frame buffers. A sensor
// set_framesize() afterwards changes what the SENSOR emits while the driver's
// DMA layout stays exactly where init left it. Going down is survivable (fewer
// bytes than expected); coming back up, frames simply stop completing, and every
// esp_camera_fb_get() burns its full ~4 s timeout before returning nothing.
//
// esp_camera_reconfigure() is the supported way to change frame size, but it is
// a deinit/init underneath: it resets the sensor and discards every exposure and
// tone setting the metering just worked out, which defeats the point.
//
// So: one resolution, chosen at init, for the whole capture.
// -----------------------------------------------------------------------------

// The metering thumbnail is the JPEG decoded at 1/8 scale. UXGA/8 = 200x150
// RGB565 = 60 KB; 128 KB also covers the OV3660's full QXGA (256x192 = 98 KB) if
// CAM_FRAME_SIZE is ever raised. Allocated per capture and wiped before it is
// freed, so no decoded image data outlives the capture that produced it.
#define CAM_METER_BUF_BYTES   (128 * 1024)

// Convergence. The loop stops as soon as two consecutive frames agree on three
// things at once — brightness, colour, and the scene not moving — which in a
// bright room with a steady hand happens in 2-3 frames. The frame cap only
// binds in hard light or when the camera is genuinely in motion.
#define CAM_SETTLE_MAX_FRAMES 10

// Brightness agreement, in luma out of 255.
#define CAM_AE_STABLE_DELTA   4.0f

// Colour agreement, as the frame-to-frame change in the R/G and B/G channel
// ratios summed together.
//
// This is a separate condition because auto-white-balance converges noticeably
// slower than auto-exposure, and a cold-start frame grabbed the moment the
// brightness stops moving is routinely still colour-cast — which is the
// difference between "the cable is blue" and "the cable is grey". Watching luma
// alone, as this pipeline used to, cannot see that at all.
#define CAM_AWB_STABLE_DELTA  0.05f

// Stillness, as the mean per-tile luma change across an 8x8 grid laid over the
// metering thumbnail. Frame-to-frame scene motion is the best available
// predictor of motion blur in the shutter frame, and on a handheld or worn
// camera motion blur is the single most common reason a label comes back
// unreadable. Measured from thumbnails the pipeline is already decoding, so
// waiting for the scene to hold still costs nothing but frames.
//
// Raise it if captures feel sluggish in the hand; lower it if blurry frames are
// still getting through. Set CAM_STILL_ENABLE to 0 to ignore motion entirely.
#define CAM_STILL_ENABLE      1
#define CAM_STILL_DELTA       6.0f

// Where a well-exposed frame should sit. 118 is a touch above the classic 18%
// grey point: Gemini reads text and labels more reliably out of a slightly bright
// frame than a slightly dark one, and the sensor's own AE aims low indoors.
#define CAM_AE_TARGET_LUMA    118.0f
#define CAM_AE_TARGET_TOL     18.0f

// How many exposure-bias steps to spend pulling the frame toward that target.
// The bias knob is +/-2, so 2 steps can traverse it from either end.
#define CAM_AE_TRIM_STEPS     2

// -----------------------------------------------------------------------------
// Auto HDR
//
// Not multi-frame radiance fusion — merging exposures would mean decoding two
// full UXGA frames to RGB888 (1.4 MB each), fusing them, and re-encoding JPEG in
// software, which costs well over a second of CPU and throws away the sensor's
// hardware encoder. What this does instead is the half that actually matters on
// a fixed-lens sensor with an 8-bit pipeline:
//
//   1. measure the scene's real dynamic range from a luma histogram,
//   2. pick the exposure that clips the fewest highlights (blown highlights are
//      gone for good; crushed shadows are partly recoverable),
//   3. reshape the sensor's own tone curve — gamma, contrast, brightness — to
//      fit that range into 8 bits instead of letting it clip at both ends.
//
// Steps 2 and 3 run in the sensor DSP at zero CPU cost, and step 2's exposure
// bracket is shot at the metering size, so the whole thing costs a handful of
// small frames rather than a second of encoding.
// -----------------------------------------------------------------------------
#define CAM_HDR_ENABLE        1

// Scene classification thresholds, all read off the 32-bin luma histogram.
// A scene counts as high dynamic range when it is clipping highlights AND still
// spans most of the range — bright sky through a window, a lit screen in a dim
// room, a hand held up against a lamp.
#define CAM_HDR_HI_CLIP       0.04f   // fraction of pixels pinned at white
#define CAM_HDR_RANGE_MIN     160     // p95 - p5 luma spread, 0-255
#define CAM_HDR_DARK_P95      90      // whole frame below this = dark scene
#define CAM_HDR_FLAT_RANGE    60      // spread below this = flat, low-contrast

// Exposure-bracket scoring weights. Highlight clipping is punished ~3x harder
// than shadow clipping because gamma can lift a shadow and nothing can recover
// a blown highlight; the midtone term only breaks ties between clean exposures.
#define CAM_HDR_W_HI          3.0f
#define CAM_HDR_W_LO          1.0f
#define CAM_HDR_W_MID         0.6f

// Bias offsets tried against the settled exposure, in order. Both are negative:
// the bracket exists to find headroom for highlights, and the AE trim above has
// already handled "too dark".
#define CAM_HDR_BRACKET       { -1, -2 }

// -----------------------------------------------------------------------------
// Autofocus
//
// Both sensors the Sense board ships with — OV2640 on older units, OV3660 on
// current ones — are fixed-focus, with no VCM to drive and no AF hardware at
// all. On those this phase is a single query to the driver and an immediate
// skip, so leaving it enabled costs nothing.
//
// On a third-party OV5640 module, which does have a VCM, it downloads the AF
// firmware, runs one focus cycle, and waits for the lens to report focused
// before the shutter frame. That is what makes reading a label held up close
// work at all: a fixed-focus lens is sharp at arm's length and nowhere else.
// -----------------------------------------------------------------------------
#define CAM_AF_ENABLE         1
#define CAM_AF_TIMEOUT_MS     600

// -----------------------------------------------------------------------------
// Shutter: best-of-N sharpness selection
//
// The stillness gate above watches the scene up to the moment the pipeline
// commits, but there is still a gap — the resolution change and, on an OV5640,
// the autofocus cycle — between the last frame it measured and the frame that
// actually gets sent. A hand can move in that gap.
//
// So the shutter takes N full-resolution frames and keeps the sharpest, scored
// by the variance of the Laplacian over the frame decoded at 1/4 scale. That is
// the standard focus/blur measure: it reads how much high-frequency detail
// survived, which is exactly what motion blur and defocus destroy and exactly
// what a model needs in order to read small print.
//
// THIS IS THE MOST EXPENSIVE OPTION IN THIS FILE. Each extra candidate costs a
// full-resolution grab plus a JPEG decode — budget ~400 ms for the default of 2.
// It is worth that when RIO is being asked to read something, because a blurred
// frame does not fail cheaply: it costs a wrong answer plus the whole spoken
// round trip to ask again. Set CAM_SHOT_CANDIDATES to 1 to turn the entire
// mechanism off and shoot a single frame, which is what the pipeline did before.
// -----------------------------------------------------------------------------
// 1200 rather than 700: at ~350-400 ms per UXGA frame, the first shot alone
// costs CAM_FLUSH_FRAMES+1 frames and blew straight past a 700 ms budget, so the
// second candidate was never taken and the sharpness comparison never happened.
// If the log only ever shows "shot 1", this is the number that is too small.
#define CAM_SHOT_CANDIDATES   2
#define CAM_SHOT_BUDGET_MS    1200

// Scratch for the 1/4-scale sharpness decode. UXGA/4 = 400x300 RGB565 = 234 KB.
// A frame too large to decode into this simply skips scoring and is taken as-is,
// so raising CAM_FRAME_SIZE degrades this feature rather than breaking it.
#define CAM_SHARP_BUF_BYTES   (256 * 1024)

// -----------------------------------------------------------------------------
// Latency guard
//
// Everything above is best-effort. When the clock runs out the pipeline stops
// optimising and shoots with whatever it has converged on so far, because a
// slightly mis-exposed answer now beats a perfect one after RIO has already
// gone quiet waiting.
//
// The clock starts once the sensor is up and delivering frames, NOT on entry to
// cameraCaptureToMessage(). Init on this board runs well over a second and its
// cost varies; charging the tuning budget for it meant the budget was gone
// before the first metering frame arrived, and the settle loop ran on a single
// unconverged sample. The shutter phase has its own CAM_SHOT_BUDGET_MS.
//
// MEASURED on the XIAO Sense + OV3660 at 10 MHz XCLK: a UXGA frame costs
// roughly 350-400 ms and init roughly 1.5 s. Everything here is frame-time
// bound, so budgets are really "how many frames am I willing to spend".
//
// 1600 buys about four UXGA frames — enough for the settle loop to see two
// consecutive frames agree (its minimum is three) with one spare for an
// exposure trim. Raising XCLK to 20 MHz roughly halves frame time and makes
// this budget worth twice as many frames.
// -----------------------------------------------------------------------------
#define CAM_TUNE_BUDGET_MS    1600

// The name the model calls. Must match the declaration sent in setup.
#define CAM_TOOL_NAME      "capture_image"


// How long a granted-but-unused permission stays open. Long enough for the user
// to hear the question and answer it, short enough that a "yes" cannot be
// harvested much later for an unrelated request.
//
// 60 s rather than 30: a spoken request-and-answer round trip through the model
// routinely runs past half a minute, and expiring mid-exchange makes RIO ask
// for permission it was already given.
#define CAM_CONSENT_WINDOW_MS  60000

// =============================================================================
// Music playback (JioSaavn, resolved and decoded entirely on-device)
//
// WHY NOT YOUTUBE, AND WHY THERE IS NO LONGER A HELPER
//
// This used to search YouTube here and hand the video id to a companion box
// running yt-dlp and ffmpeg. Both halves of that are gone, for one reason each.
//
// The device could never resolve YouTube audio, and the reason got worse rather
// than better. It is no longer a matter of descrambling `signatureCipher`:
// MEASURED on a live watch page, `streamingData` now carries NO per-format url
// and NO signatureCipher at all — only `serverAbrStreamingUrl`. Fetching bytes
// means speaking SABR: POST a protobuf VideoPlaybackAbrRequest, parse a UMP
// framed response, and carry a PO token minted by executing YouTube's obfuscated
// BotGuard JavaScript. That last part needs a JS VM, which settles it. The old
// InnerTube shims are closed too — the IOS and ANDROID clients answer HTTP 400
// FAILED_PRECONDITION, TVHTML5 answers "no longer supported", and public
// Invidious instances answer 403.
//
// So the source moved. JioSaavn hands back a plain AAC-LC file over plain HTTP,
// which is a thing this chip can genuinely play, and that removed the reason for
// the helper to exist: with no signature to descramble and no transcode needed,
// there was nothing left for a second machine to do. See jiosaavn.h for the
// lookup and mp4aac.h for the demux and decode.
//
// WHAT THE DEVICE NOW DOES PER SONG
//
//   1. One HTTPS GET to the JioSaavn search API (~34 KB) — jiosaavn.cpp.
//   2. DES-ECB decrypt of `encrypted_media_url` via mbedtls, already linked.
//   3. One plain-HTTP GET of the .mp4 from the CDN. No second TLS session.
//   4. Buffer and parse `moov`, then stream `mdat` frame by frame — mp4aac.cpp.
//   5. Decode AAC-LC, downmix to mono, resample to SPK_SAMPLE_RATE.
//   6. Push into gPcmOut, exactly where the helper's PCM used to land.
//
// Step 6 is unchanged on purpose. spkTask already calls aecPushReference() on
// everything it plays, so music remains the echo canceller's reference for free,
// which is what lets the user talk OVER a song. A separate playback path would
// have had to duplicate that, and audio_out.cpp's staging buffer is
// single-writer besides (see the note above STAGE_MONO_SAMPLES).
// =============================================================================

// Tool names. Must match the declarations sent in setup.
#define MUSIC_TOOL_NAME       "play_song"
#define MUSIC_STOP_TOOL_NAME  "stop_song"

// Longest song name accepted from the model. Sized for a title plus an artist
// ("gehra hua arijit singh"), not for a sentence.
#define MUSIC_QUERY_MAX       96

// Longest resolved track title kept for logging.
#define MUSIC_TITLE_MAX       96

// -----------------------------------------------------------------------------
// JioSaavn lookup
// -----------------------------------------------------------------------------
#define JIOSAAVN_HOST      "www.jiosaavn.com"
#define JIOSAAVN_PORT      443

// One call does everything. MEASURED: search.getResults with n=1 returns ~34 KB
// and already contains `encrypted_media_url` for the top hit at byte ~677, so
// the song.getDetails round trip the API also offers is not needed.
//
// api_version=4 and ctx=web6dot0 are not decoration — drop them and the response
// comes back in a different, older shape without the media url.
#define JIOSAAVN_API_PATH \
    "/api.php?_format=json&_marker=0&api_version=4&ctx=web6dot0" \
    "&__call=search.getResults&n=1&q="

// The response is buffered whole rather than scanned as it streams, because at
// 34 KB it fits with room to spare and strstr is far less fiddly than a set of
// streaming matchers. A truncated read is survivable: the top result's fields
// land in the first kilobyte and the top result is all this ever reports.
#define JIOSAAVN_RX_MAX      (96UL * 1024UL)
#define JIOSAAVN_TIMEOUT_MS  20000
#define JIOSAAVN_URL_MAX     256

// The fixed DES-ECB key the service obfuscates media URLs with. This is a
// constant of the service, not a secret of ours, which is why it sits in
// config.h and not secrets.h.
#define JIOSAAVN_DES_KEY     "38346591"

// Which CDN rendition to ask for: "_12", "_48", "_96", "_160" or "_320".
//
// MEASURED, all of them are plain AAC-LC, so this only trades bandwidth and
// sample rate. _96 is 44.1 kHz stereo at ~98 kbps (12 KB/s) and is the default.
//
// Going above _96 is wasted: the output is mono at SPK_SAMPLE_RATE, which caps
// the usable bandwidth at 12 kHz, and the extra bits are discarded by the
// resampler. _48 is 22.05 kHz — half the data, and the ONLY setting where the
// resampler is alias-free, because the source is already band-limited below the
// output Nyquist. Switch to it if _96 ever sounds harsh.
#define JIOSAAVN_QUALITY     "_96"

// -----------------------------------------------------------------------------
// CDN streaming and decode
// -----------------------------------------------------------------------------
#define MUSIC_CDN_PORT         80

// The whole `moov` box is buffered so the sample tables can be parsed at once.
// MEASURED: 63,283 bytes on a 6-minute _96 track, of which 62,456 is stsz. The
// table grows with duration — roughly 4 bytes per 1024 samples — so 256 KB
// covers about 25 minutes at 44.1 kHz. PSRAM, so the headroom is cheap.
#define MUSIC_MOOV_MAX         (256UL * 1024UL)

// Sanity bound on a single AAC frame. Real frames run a few hundred bytes; this
// exists so a corrupt stsz cannot talk the reader into a huge allocation.
#define MUSIC_MAX_FRAME_BYTES  8192

// Socket read granularity on the CDN. Also bounds how much decoded audio can
// still land after a barge-in, since the stop flag is checked once per frame.
#define MUSIC_RX_CHUNK         4096

// Music is mixed into the same buffer as RIO's voice, and it is the reference
// the echo canceller has to work against. Loud music is the fastest way to drive
// ERLE into the floor and have RIO stop hearing you — turn this DOWN, not up, if
// barge-in during a song stops working.
#define MUSIC_VOLUME           0.55f

// How long the CDN may go silent mid-song before the stream is abandoned.
#define MUSIC_STALL_TIMEOUT_MS 8000

// How long to wait for the CDN to answer at all. A static file server, so this
// is far shorter than the helper's old yt-dlp-and-ffmpeg spin-up allowance.
#define MUSIC_CONNECT_TIMEOUT_MS 10000
#define MUSIC_HEADER_TIMEOUT_MS  10000

// How many consecutive-ish frame decode failures to tolerate before giving up
// on a track. A few bad frames in a multi-thousand-frame file is a scratch on
// the CDN copy and not worth abandoning the song over; hundreds means the
// stream is not the AAC-LC we set the decoder up for.
#define MUSIC_MAX_DECODE_ERRORS 64

// How many times a broken connection may be re-established mid-song before
// the track is abandoned. A four-minute song is four minutes of WiFi, and
// aac.saavncdn.com honours Range requests (MEASURED: 206 Partial Content), so
// a dropped socket is recoverable rather than fatal. Bounded so a CDN that is
// refusing to serve does not become an infinite reconnect loop.
#define MUSIC_MAX_RESUMES      6

// Requests waiting to be played. One: a second "play X" supersedes the first
// rather than queueing an album nobody asked for.
#define MUSIC_QUEUE_DEPTH      1

// musicTask runs the TLS lookup, the streaming loop and the AAC decoder, so it
// is sized like netTask rather than like an audio task. The decoder keeps its
// own buffers off-stack (see mp4aac.cpp), so this covers mbedtls, not codec.
#define MUSIC_TASK_STACK       10240

// -----------------------------------------------------------------------------
// Memory admission control
//
// Internal (DRAM) heap is the binding constraint on this build, not PSRAM.
// MEASURED on hardware: a JioSaavn TLS handshake running alongside the live
// Gemini WebSocket took free internal heap from ~90 KB down to 676 bytes. It
// survived, but with no margin at all.
//
// mbedtls wants two ~16 KB record buffers plus the session, certificate parsing
// and verification state. MEASURED, the whole handshake costs on the order of
// 80-90 KB internal at peak, transiently.
//
// MUSIC_LOOKUP_INTERNAL_NEED is the bar a lookup must clear before it is even
// attempted. Set BELOW the measured peak on purpose: mbedtls frees as it goes
// and the true simultaneous peak is lower than the total churn, so demanding
// the full 90 KB would refuse lookups that would have worked. Raise it if you
// see reboots without a task_wdt banner; lower it if songs are declined while
// the [MEM] line still shows healthy numbers.
// -----------------------------------------------------------------------------
#define MUSIC_LOOKUP_INTERNAL_NEED  (56UL * 1024UL)

// Same idea before powering the camera: esp_camera_init() takes DMA descriptors
// and line buffers from internal RAM, and a half-initialised sensor is a much
// worse outcome than a spoken "not right now".
#define CAM_INIT_INTERNAL_NEED      (40UL * 1024UL)

// =============================================================================
// News headlines (Google News RSS search, resolved entirely on-device)
//
// The RSS feed at NEWS_HOST+NEWS_RSS_PATH is the machine-readable twin of the
// browser search page at https://news.google.com/search?q=...&hl=en-IN&gl=IN
// &ceid=IN:en — same query parameters, but each result's <title> ("Headline -
// Source") is plain XML text instead of something a JS-rendered page hides.
// -----------------------------------------------------------------------------

// The name the model calls. Must match the declaration sent in setup.
#define NEWS_TOOL_NAME        "get_news"

// Longest topic accepted from the model, and longest one headline title kept.
#define NEWS_QUERY_MAX        96
#define NEWS_TITLE_MAX        100

#define NEWS_HOST             "news.google.com"
#define NEWS_PORT             443

// hl/gl/ceid pin the edition to English results targeted at India, matching
// the browser link this was built from. Change these to change the edition.
#define NEWS_RSS_PATH         "/rss/search?q="
#define NEWS_RSS_SUFFIX       "&hl=en-IN&gl=IN&ceid=IN:en"

// Buffered incrementally and stopped early once NEWS_MAX_HEADLINES <item>s
// have arrived — see news.cpp. The feed lists results most-relevant-first, so
// this only ever costs headlines nobody was going to hear anyway.
#define NEWS_RX_MAX           (16UL * 1024UL)
#define NEWS_TIMEOUT_MS       15000

// How many headlines to read out. Kept short on purpose: this is spoken back
// in one or two sentences, not read as a list.
#define NEWS_MAX_HEADLINES    3

// Combined "headline; headline; headline" text handed to the model. Sized so
// "Recent headlines about <topic>: <this>" always fits sendModelNote's escape
// buffer in gemini_live.cpp even at NEWS_QUERY_MAX.
#define NEWS_HEADLINES_MAX    260

// Requests waiting to be looked up. One: a second topic supersedes the first
// rather than queueing lookups nobody is still waiting on.
#define NEWS_QUEUE_DEPTH      1

// newsTask runs only the TLS lookup, no decoder, so this is smaller than
// MUSIC_TASK_STACK.
#define NEWS_TASK_STACK       10240

// Same reasoning as MUSIC_LOOKUP_INTERNAL_NEED: this opens its own TLS session
// against news.google.com while the Gemini WebSocket is live.
#define NEWS_LOOKUP_INTERNAL_NEED  (56UL * 1024UL)

// =============================================================================
// Holidays (Simpliance state-wise labour-law holiday list)
//
// WHY NOT india.gov.in/calendar
//
// That was the first thing tried, since it's the official source and the one
// this was originally asked for. It does not work: the page is a
// client-rendered Next.js app, and the raw HTML carries zero holiday text —
// only a JS shell that fetches the list after loading in a browser. MEASURED
// directly: the initial response has no date-like text anywhere in it, and
// grepping the page's own bundled JS for a REST/JSON/ICS endpoint under that
// domain, plus capturing live network traffic with headless Chrome, found no
// URL that hands holiday data back to a plain HTTPS client. An ESP32 doing a
// bare GET has no JS engine, so no request built for that domain can work.
//
// Simpliance's per-state page is the replacement:
//
//     https://www.simpliance.in/India/LEI/holiday_list/<state-slug>/<year>
//
// genuinely server-rendered — each row of its table carries
// data-holiday-name/date/type attributes directly in the HTML, which holiday.cpp
// parses with the same byte-scanning approach jiosaavn.cpp uses for JSON.
// -----------------------------------------------------------------------------

// The name the model calls. Must match the declaration sent in setup.
#define HOLIDAY_TOOL_NAME        "get_holidays"

// Longest state name accepted from the model (slugified into the URL), and
// longest single holiday name kept from a row.
#define HOLIDAY_STATE_MAX        64
#define HOLIDAY_NAME_MAX         48

// Used when the model calls the tool with no state at all.
#define HOLIDAY_DEFAULT_STATE    "Bihar"

#define HOLIDAY_HOST             "www.simpliance.in"
#define HOLIDAY_PORT             443
#define HOLIDAY_PATH_PREFIX      "/India/LEI/holiday_list/"

// MEASURED: the Bihar page for 2026 is ~250 KB, and the holiday table's rows
// start past the 175 KB mark — this buffers the response in full rather than
// stopping early (see holiday.h), so it has to be sized well past the whole
// page rather than just the first few KB the way NEWS_RX_MAX is.
#define HOLIDAY_RX_MAX           (320UL * 1024UL)
#define HOLIDAY_TIMEOUT_MS       20000

// How many upcoming holidays to read out. A lookup may fetch two pages (this
// year, then next) if none remain in the current year — see holiday.cpp.
#define HOLIDAY_MAX_RESULTS      3

// Combined "Name (DD Mon); Name (DD Mon); ..." text handed to the model.
#define HOLIDAYS_TEXT_MAX        260

// Requests waiting to be looked up. One: a second state supersedes the first.
#define HOLIDAY_QUEUE_DEPTH      1

// holidayTask runs only the TLS fetch, no decoder, same size as NEWS_TASK_STACK.
#define HOLIDAY_TASK_STACK       10240

// Same reasoning as MUSIC_LOOKUP_INTERNAL_NEED: this opens its own TLS session
// against simpliance.in while the Gemini WebSocket is live.
#define HOLIDAY_LOOKUP_INTERNAL_NEED  (56UL * 1024UL)

// -----------------------------------------------------------------------------
// System prompt
//
// Deliberately a single line with NO double quotes and NO newlines, so it can be
// embedded in hand-built JSON with zero escaping. Keep it that way when editing:
// a stray quote here produces a malformed setup message that is very hard to
// diagnose on-device.
//
// Kept deliberately short. Every tool's mechanics — the camera consent handshake,
// what the play_song result strings mean — are already spelled out in the
// functionDeclarations sent alongside this in sendSetup(), so repeating them here
// only spends tokens and invites the two copies to drift apart. What stays are
// the rules the declarations cannot state: tone, brevity, language, and that
// RIO speaking stops the music.
// -----------------------------------------------------------------------------
#define SYSTEM_INSTRUCTION \
  "You are RIO, a warm, witty female voice assistant on a live call. " \
  "Reply in the user's language - Hindi, English or Hinglish - in one or two spoken sentences. No lists, markdown, emoji or URLs. " \
  "Never falsely claim to see, play or do anything a tool has not confirmed; if one fails, say so briefly. " \
  "Use a tool only when needed: capture_image only to look at something now, and only once the user agrees; " \
  "play_song for music, confirmed in three words, then stay silent because anything you say stops the song; stop_song only when asked to stop; " \
  "get_news for current news or events on a topic, confirmed briefly, then read out the headlines once they arrive; " \
  "get_holidays for upcoming holidays, in a named state if the user said one, confirmed briefly, then read them out once they arrive. " \
  "Use what the user already said; if unclear, ask one short question. " \
  "Sound human - cheerful, gentle, playful or calm to match their mood, with at most one touch like Oh or Hmm. " \


// The trailing clause matters because sessionResumption replays earlier turns:
// old sessions carry their own now-stale timestamp, and without it the model has
// two contradictory nows in context and no way to rank them.
#define TIME_PROMPT_PREFIX  " Right now it is "
#define TIME_PROMPT_SUFFIX  ". Trust this over any earlier time mentioned in this conversation."
#define TIME_PROMPT_FORMAT  "%A %d %B %Y, %H:%M IST"

#endif  // CONFIG_H
