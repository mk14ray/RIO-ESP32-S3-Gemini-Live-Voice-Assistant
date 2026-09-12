# RIO — standalone Gemini Live voice assistant

Firmware for a self-contained voice assistant: onboard mic captures speech,
streams it to Google's Gemini Live API over `wss://`, and plays the spoken
reply back through an external amp/speaker. No PC or phone in the loop at
runtime — only WiFi.

```
onboard PDM mic (I2S0, 16 kHz) --> base64 JSON frames --> Gemini Live
Gemini Live --> base64 PCM (24 kHz) --> MAX98357A amp (I2S1) --> speaker
camera (DVP) --> JPEG --> base64 --> Gemini Live   [only on granted permission]
```

**Full duplex**: the mic stays open while RIO is talking, so you can interrupt
mid-reply and speech can overlap naturally. That is only possible because
acoustic echo cancellation removes RIO's own voice from the mic feed — see
[Full duplex](#full-duplex). Set `FULL_DUPLEX 0` in [config.h](config.h) to
fall back to the original half-duplex behaviour.

For visual questions RIO can take a still with the onboard camera, but only
after asking and being answered — see [Camera permission](#camera-permission).

## Hardware

### MCU: Espressif ESP32-S3

- Xtensa **LX7 dual-core** @ up to 240 MHz, plus a low-power RISC-V coprocessor.
- 512 KB SRAM on-chip; this board adds 8 MB external **PSRAM** (required —
  see `appStateBegin()` in [app_state.cpp](app_state.cpp), which allocates every
  large buffer from PSRAM and fails setup if it's not present).
- WiFi 802.11 b/g/n (2.4 GHz) + Bluetooth 5 LE. Only WiFi is used here
  ([wifi_mgr.h](wifi_mgr.h)).
- Two I2S controllers. **PDM RX (microphone) is only supported on I2S0** on
  the S3 — this is why the mic is hard-pinned to `I2S_NUM_0` and the speaker
  to `I2S_NUM_1` in [config.h](config.h), rather than letting the driver pick
  a port automatically.
- Hardware TLS/crypto acceleration, used implicitly by `WiFiClientSecure` for
  the `wss://` connection to Gemini.

Datasheet: https://documentation.espressif.com/esp32-s3_datasheet_en.pdf

### Board: Seeed Studio XIAO ESP32S3 Sense

Thumb-sized (21 × 17.5 mm) ESP32-S3 dev board in the XIAO form factor, with a
"Sense" expansion piggybacked on top.

| | |
|---|---|
| MCU | ESP32-S3R8 (see above) |
| Memory | 8 MB PSRAM + 8 MB flash on-chip |
| Storage | microSD slot on the Sense expansion, up to 32 GB FAT |
| Wireless | 2.4 GHz WiFi + BLE 5.0 |
| Sense sensors | 1× camera (OV2640 on older units, **OV3660 on current units** — OV2640 was discontinued and silently swapped; camera driver code is compatible with both, and the unit this was developed against is an OV3660) + 1× onboard PDM digital microphone |
| Breakout | 11× GPIO (PWM), 9× ADC, 1× UART, 1× I2C, 1× SPI, 1× user LED, reset + boot buttons |
| Power | USB-C or LiPo (JST), 4 power modes down to ~14 µA in deep sleep |

This project uses the **onboard PDM microphone** and the **onboard camera**
from the Sense module. The microSD slot is present but unused.

Pin map actually wired ([config.h](config.h)):

| Signal | XIAO pin | GPIO | Notes |
|---|---|---|---|
| PDM mic clock | (onboard, Sense module) | 42 | fixed, not on the general breakout |
| PDM mic data | (onboard, Sense module) | 41 | fixed, not on the general breakout |
| Speaker BCLK | D8 | 7 | to MAX98357A |
| Speaker LRC (word select) | D9 | 8 | to MAX98357A |
| Speaker DIN | D10 | 9 | to MAX98357A |
| Status LED | — | 21 | onboard user LED, **active LOW** |
| Camera XCLK | (B2B connector) | 10 | fixed by the Sense board |
| Camera SCCB SDA / SCL | (B2B) | 40 / 39 | sensor control, I2C-like |
| Camera data D0–D7 | (B2B) | 15, 17, 18, 16, 14, 12, 11, 48 | 8-bit DVP parallel bus |
| Camera VSYNC / HREF / PCLK | (B2B) | 38 / 47 / 13 | |

The camera pins are all on the Sense board's B2B connector and none of them
collide with the mic (41/42), the speaker (7/8/9) or the LED (21), so all
three subsystems coexist without remapping anything.

D8/D9/D10 double as the XIAO's hardware SPI pins, which the Sense board also
uses for its microSD slot — so on this board the speaker and the SD card are
mutually exclusive. Not an issue here since the SD card isn't used.

Repo used as reference for board bring-up/tutorials:
https://github.com/Mjrovai/XIAO-ESP32S3-Sense
(Seeed's official specs/wiki: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)

### Camera sensor: OV2640 / OV3660 (on the Sense module)

- 1/4" CMOS, rolling shutter. OV2640 tops out at 1600×1200 (UXGA); the OV3660
  goes to 2048×1536 (QXGA). This firmware asks for UXGA on either — see the
  `CAM_FRAME_SIZE` note in [config.h](config.h) for why more is not better here.
- **DVP** parallel interface for pixel data (8-bit + H-sync/V-sync/PCLK) —
  simple enough for low-end MCUs, unlike MIPI.
- **SCCB** (I2C-like) for control/configuration.
- On-chip ISP: auto-exposure, auto-white-balance, and a hardware **JPEG
  encoder** — the latter is why it was historically popular on memory-poor
  microcontrollers (a full UXGA RGB565 frame is ~3.66 MB; JPEG shrinks that
  ~25×).
- Note: newer XIAO ESP32S3 Sense units ship an **OV3660** in place of the
  OV2640 (the OV2640 was discontinued). This costs nothing here: the
  `esp32-camera` driver identifies the sensor over SCCB at init, and
  [camera.cpp](camera.cpp) queries it for everything that differs — maximum
  frame size, whether `set_ae_level` is implemented, whether autofocus exists —
  rather than assuming. It works unmodified on either. Don't hard-code an
  assumption about which one is fitted.

#### OV3660 settings that are not optional on this board

Two driver settings differ from every OV2640 example you will find, and getting
either wrong produces the same misleading symptom — `NO-EOI - JPEG end marker
missing`, then `Failed to get frame: timeout`, then a capture that fails with no
indication why:

| Setting | OV2640 default | Needed for OV3660 | Why |
|---|---|---|---|
| `CAM_XCLK_HZ` | 20 MHz | **10 MHz** | 20 MHz is too fast for this sensor at high resolution over the Sense B2B connector; frames stop completing |
| PSRAM DMA mode | on | **off** (`CAM_PSRAM_DMA_MODE 0`) | DMAing straight into PSRAM overflows with the OV3660 |
| `fb_count` | 1 is fine | **2** | single-buffering causes FB-OVF errors |

ESP-IDF projects set the PSRAM DMA one with `CONFIG_CAMERA_PSRAM_DMA_MODE=n` at
build time. Arduino ships a precompiled driver, so [camera.cpp](camera.cpp) uses
the runtime `esp_camera_set_psram_mode()` instead — and does it immediately after
init, because that call reinitialises the camera and would discard any tuning
applied before it.

Cost: 10 MHz XCLK roughly doubles frame time, which is why `CAM_TUNE_BUDGET_MS`
is 1400 ms rather than 900. Once captures are reliably working, raising XCLK back
to 20 MHz is the single biggest latency win available — but revert it the moment
timeouts reappear.

Reference for these values, on the same board and sensor:
https://github.com/manjotkhangura/ESP32S3-Sense-OV3660

#### The OV3660 comes up upside down

**This is a sensor defect, not a mounting question.** The OV3660's reset defaults
leave it vertically flipped and visibly oversaturated. Espressif correct for both
in their own `CameraWebServer` reference (`set_vflip(s, 1)`, `set_saturation(s, -2)`),
and [camera.cpp](camera.cpp) does the same, keyed on the PID the driver read over
SCCB. Uncorrected, every frame reaches Gemini upside down — which costs more than
any amount of exposure tuning wins back, because upside-down text is text the
model has to work to read and often just misreads.

`CAM_FLIP_VERTICAL` / `CAM_MIRROR_HORIZONTAL` in [config.h](config.h) are for
*mounting* orientation and are XORed on top of that correction. Do **not** set
`CAM_FLIP_VERTICAL` to 1 to fix an upside-down OV3660 — that correction already
happens, and setting this too flips it back the wrong way.

What else differs between the two, all handled at runtime:

| | OV2640 | OV3660 |
|---|---|---|
| Max frame size | UXGA 1600×1200 | QXGA 2048×1536 |
| `set_ae_level` | not implemented — pipeline falls back to the DSP brightness offset | implemented — real AE bias, used for the trim and the HDR bracket |
| `set_denoise` / `set_sharpness` | not implemented (NULL pointers) | implemented |
| Reset orientation | upright | **vertically flipped** |
| Autofocus | none (fixed focus) | none (fixed focus) |

Autofocus exists in `esp32-camera` only for the OV5640 (`ov5640_af.c` is the sole
AF object in the library), so on either stock sensor the AF phase is one driver
query and an immediate skip.

Reference: https://www.arducam.com/blog/ov2640/

General ESP32-CAM background (a different, cheaper AI-Thinker board — not
what this project uses, but a useful comparison point for OV2640 wiring/
quirks like ground-plane noise causing stripe artifacts at max resolution):
https://matchboxscope.github.io/docs/Hardware/ESP32Cam/

### Amplifier: MAX98357A (external module, I2S DAC + Class-D amp)

Not part of the XIAO/Sense board — a separate breakout wired to I2S1.

- Takes I2S in, outputs directly to a speaker (no separate DAC/amp chain).
- 3-wire I2S: BCLK, LRC (word select), DIN. No MCLK needed.
- With the SD (shutdown/gain-select) pin floating, it sums L+R and outputs
  (L+R)/2 — which is why [audio_out.h](audio_out.h) explicitly duplicates
  the mono stream into both stereo slots rather than using I2S "mono mode"
  (which zero-fills the unselected slot and would silently halve volume).

## Getting it running

### 1. Dependencies

| Requirement | Verified present |
|---|---|
| Arduino-ESP32 core **3.x** (ESP-IDF 5.x I2S API) | 3.3.11 |
| ArduinoJson **7.x** | 7.4.3 |

Core 3.x is not optional: [audio_in.h](audio_in.h) uses `driver/i2s_pdm.h`,
which does not exist in core 2.x. The camera driver (`esp_camera.h`) ships
with the core — nothing extra to install.

### 2. Credentials

Edit [secrets.h](secrets.h) with your WiFi SSID/password and a Gemini API key
from https://aistudio.google.com/apikey. **Rotate the keys currently in that
file before using it** — see [Security note](#security-note).

### 3. Wiring

Only the amplifier needs wiring; mic and camera are already on the board.

```
MAX98357A        XIAO ESP32-S3 Sense
  VIN    <-----  5V  (or 3V3)
  GND    <-----  GND
  BCLK   <-----  D8   (GPIO 7)
  LRC    <-----  D9   (GPIO 8)
  DIN    <-----  D10  (GPIO 9)
  SD     <-----  leave floating
```

Leaving SD floating is intentional — see [audio_out.h](audio_out.h) for why
the code sends real stereo rather than I2S mono mode.

### 4. Board settings

In the Arduino IDE, pick *Tools > Board > esp32 >* **XIAO_ESP32S3** — not
"ESP32S3 Dev Module". The XIAO profile sets flash size, partition scheme and USB
CDC correctly on its own; the generic Dev Module profile defaults to 4 MB flash,
which is wrong for this board and leaves the sketch at **88 %** of a 1.3 MB app
partition instead of 34 % of 3.3 MB.

Then set *Tools > PSRAM >* **OPI PSRAM**. **It defaults to Disabled and nothing
here works without it** — this is the single most common bring-up failure.

Serial Monitor must be at **115200** to match `Serial.begin()`.

### Troubleshooting: StoreProhibited panic at boot

```
Guru Meditation Error: Core 1 panic'ed (StoreProhibited)
EXCVADDR: 0x00000000   A6: 0x00000404
... dios_ssp_aec_firfilter_init
```

**PSRAM is disabled.** The echo canceller allocates with
`caps = 0x404` (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`) — visible as `A6` in the
register dump — and esp-sr does not check the result, so with no PSRAM heap it
stores through a NULL pointer and boot-loops. Set PSRAM to OPI PSRAM.

setup() now tests for PSRAM before anything allocates, so a board in this state
reports it in words and halts rather than panicking. If you see the panic
anyway, you are running an older build.

With arduino-cli the setting is part of the FQBN:

```bash
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi
arduino-cli board list                       # find the port
arduino-cli upload -p /dev/cu.usbmodemXXXX --fqbn esp32:esp32:XIAO_ESP32S3:PSRAM=opi
arduino-cli monitor -p /dev/cu.usbmodemXXXX -c baudrate=115200
```

If the port doesn't appear, double-tap RESET to enter the bootloader.

### 5. Bring-up, one layer at a time

Set `BRINGUP_MODE` in [rio_assistant.ino](rio_assistant.ino), flash, and work
down the list. A failure then points at one subsystem instead of the whole
stack.

| Mode | Proves | Expect |
|---|---|---|
| `BRINGUP_TONE` | speaker wiring + I2S1 | 440 Hz beep every 2 s |
| `BRINGUP_MIC` | mic + I2S0 PDM | `rms=` near 0 in silence, hundreds–thousands when speaking |
| `BRINGUP_LOOPBACK` | both directions | your voice echoed back (howling is expected and normal) |
| `BRINGUP_AEC` | echo cancellation | `erle` > 20 dB — see [Calibrating AEC_REF_DELAY_MS](#calibrating-aec_ref_delay_ms) |
| `BRINGUP_NONE` | — | normal operation |

`BRINGUP_AEC` is not optional if you are running full duplex: an uncalibrated
`AEC_REF_DELAY_MS` is the difference between natural conversation and RIO
interrupting itself continuously.

`BRINGUP_MIC` is also how you tune `MIC_GAIN_FACTOR`. If quiet-room RMS is
already high, lower it — Gemini's VAD will otherwise trigger on room noise.

### 6. Using it

On boot you should see the banner, `[MEM]` allocations, then:

```
[GEMINI] setupComplete
[STATE] listening — say something
```

The LED is lit while listening or speaking. Just talk — turn-taking is handled
by Gemini's server-side VAD, so there is no wake word and no button. It replies
in whichever of English/Hindi/Hinglish you used.

Every 10 s a telemetry line prints; these are the numbers worth watching:

```
[STAT] LISTENING  wifi=up rssi=-52dBm heap=... psram=... | stack mic=... spk=... net=...
       | drops=0 ovf=0 recon=0
```

- `drops` climbing during speech → mic chunks aren't reaching the network in
  time; `ovf` climbing → a server message exceeded `WS_RX_ASSEMBLY_BYTES`.
- `net=` is netTask's remaining stack headroom. Camera capture runs on that
  task, so check it after your first photo.

**To use the camera, just ask a question that needs eyes** — "what am I
holding?", "read this label for me", "what colour is this?". RIO will ask
permission first and wait for your answer; say yes and it takes the shot. You
should see this in the log:

```
[GEMINI] <-- toolCall capture_image
[CAM] permission requested — camera stays off until the user answers
[GEMINI] --> toolResponse capture_image: permission_required
[RIO] Should I take a look with the camera?
[YOU] yes go ahead
[GEMINI] <-- toolCall capture_image
[CAM] permission granted — powering up sensor
[CAM] captured 800x600, 41213 B jpeg -> 55024 B message
[GEMINI] --> toolResponse capture_image: image_captured
```

If you see `[CAM] still refused (asked=0 answered=1)` the gate is working as
designed — the model tried to reuse an old turn as consent. Ordinary questions
never produce a `toolCall` line at all, which is the quickest way to confirm
the camera stays out of non-visual conversations.

## Full duplex

The mic is never muted. The user can talk over a reply, cut RIO off mid-
sentence, and take the floor back without waiting — the turn-taking itself is
performed by Gemini's server-side VAD, which emits `interrupted` the moment it
hears the user during a model turn, and playback is flushed on the spot.

The hard part is local, not remote. The speaker is centimetres from the mic,
so an open mic hears RIO far louder than it hears the user. Left alone, the
VAD reads that as a barge-in and **the assistant interrupts itself in a loop** —
or worse, decides the user never stops talking and stops taking turns at all.

The mic front end is three stages, in [aec.cpp](aec.cpp):

```
spkTask ── PCM 24k ──┬──> I2S1 ──> amp ──> speaker ──┐
                     │                               │ acoustic echo
                     └─> 3:2 decimate ──> delay line │
                                             │       v
                                          reference  mic (I2S0)
                                             │       │
                                             v       v
                                    ┌──────────────────────┐
                                    │ 1. AEC    cancel     │
                                    │ 2. NS     de-noise   │
                                    │ 3. GATE   is this    │
                                    │           really the │
                                    │           user?      │
                                    └──────────┬───────────┘
                                               v
                                   micTask ──> Gemini (or silence)
```

**Stage 3 is what makes this robust rather than merely good when tuned.**
While RIO is audible, a frame is forwarded only if the local VAD calls it
speech *and* it stands clear of the echo predicted from the **measured** ERLE:

```
bar = mic_rms × 10^(−ERLE/20) × AEC_GATE_MARGIN
forward only if  speech  AND  out_rms > bar
```

Predicted from the mic level rather than the reference deliberately: the two
are in different domains (digital playback vs. whatever the acoustic path and
mic gain deliver) and the coupling between them is unknown. ERLE is by
definition the mic-to-output ratio during echo-only stretches, so this needs no
coupling estimate.

The bar moves with how well cancellation is actually working:

| Measured ERLE | Behaviour |
|---|---|
| **≥ ~12 dB** | user's voice clears the bar — real barge-in, full duplex |
| **< ~12 dB** | nothing clears it while RIO speaks — mic effectively muted, **half duplex** |
| **0 dB (AEC dead)** | bar reaches the mic level itself, which cancellation can never exceed — gate simply stays shut |

So the failure mode is *"loses barge-in"*, never *"talks to itself"*. An
uncalibrated or drifting delay costs a feature instead of breaking the
conversation. Verified across the full ERLE range: echo does not leak at any
value, including 0 dB.

`gate=shut` in the `[MIC]` line while RIO speaks is the system working
correctly. `gated=` in `[STAT]` counts frames it refused.

Noise suppression (stage 2, `AEC_NS_MODE`) earns its place beyond audio
quality: a high room noise floor alone can hold Gemini's server-side VAD open
permanently, which produces the failure where transcripts keep arriving but the
model never replies.

Two things have to line up, and they are the two most likely things to be
wrong:

**Rate.** The canceller runs at 16 kHz on both inputs; playback is 24 kHz. Only
the AEC's *copy* of the signal is decimated 3:2 — audio reaching the speaker is
untouched, so decimator quality costs cancellation, never playback quality. The
resampler is verified drift-free (exactly 2:3, no accumulated phase error over
30 s of continuous audio), which matters because drift would slowly pull the
reference out of alignment and quietly kill cancellation over minutes.

**Delay.** `AEC_REF_DELAY_MS` is the gap between handing a sample to the I2S
DMA and that sample turning up in a mic frame. Almost all of it is queueing, and
**both** DMA queues count — the echo has to clear the TX queue, cross the room,
then clear the RX queue:

```
TX (audio_out.cpp)  6 desc x 480 frames @ 24 kHz = 120 ms
RX (audio_in.cpp)   8 desc x 400 frames @ 16 kHz = 200 ms
acoustic flight     ~10 cm                       =  <1 ms
                                                   -------
                                                    ~320 ms
```

Change `dma_desc_num` / `dma_frame_num` in either file and this must be
re-measured. Shrinking them also cuts conversational latency, which is the
reason to consider it.

**Symptom of getting it wrong:** `erle` near 0 dB, and RIO's own sentences
coming back as `[YOU]` transcripts a moment after it says them —

```
[RIO] मुझे आपके हाथ में कुछ दिख नहीं रहा, बस आपका चेहरा
[YOU] मुझे आपके हाथ में कुछ दिख नहीं रहा। बस आपका चेहरा
```

### Calibrating AEC_REF_DELAY_MS

Set `BRINGUP_MODE BRINGUP_AEC` in [rio_assistant.ino](rio_assistant.ino) and
flash. It sweeps the delay 0–600 ms by itself, ~1 minute, and prints a table.
Stay silent and still — your voice would be scored as failed cancellation.

```
   delay_ms   erle_dB   mic_rms   out_rms
   --------   -------   -------   -------
        280      12.4      4210      1020
        300      21.8      4198       340   <-- best so far
        320      24.1      4205       262   <-- best so far
        340      15.2      4211       730

  BEST: AEC_REF_DELAY_MS = 320   (erle 24.1 dB)
```

Broadband noise is used rather than a tone on purpose: an adaptive filter
converges on whatever excites it, and a single sine tells you almost nothing
about how it will behave on speech.

| ERLE | Meaning |
|---|---|
| **> 20 dB** | healthy — full duplex will work |
| 10–20 dB | marginal; keep `START_SENSITIVITY_LOW`, try `AEC_MODE_FD_HIGH_PERF` |
| **< 10 dB** | **full duplex will not work.** Usually mechanical coupling — decouple the speaker from the board, or set `FULL_DUPLEX 0` |

Copy the winning value into [config.h](config.h), set `BRINGUP_MODE
BRINGUP_NONE`. `erle=` also appears in the `[STAT]` line during normal
operation, so you can confirm it holds up under real conditions.

### Tuning the feel

Once ERLE is healthy, in [config.h](config.h):

- `VAD_START_SENSITIVITY` → `START_SENSITIVITY_HIGH` makes interruptions feel
  instant. **Raise this only after confirming good ERLE** — with marginal
  cancellation, residual echo starts turns and RIO talks over itself.
- `VAD_SILENCE_DURATION_MS` (default 500 in full duplex, was 700) is how long a
  pause must be before your turn ends. Lower is snappier, but too low cuts you
  off while you are thinking.
- `AEC_MODE` → `AEC_MODE_FD_HIGH_PERF` if ERLE is marginal and the stack
  figures in `[STAT]` show CPU headroom.
- `AEC_NLP_LEVEL` → `VERYAGGR` suppresses more echo, but it buys that by
  chewing into the user's speech — which costs you the overlap full duplex
  exists to provide.

If you cannot get usable ERLE on your unit, `FULL_DUPLEX 0` restores the
half-duplex path unchanged; it cannot self-trigger, at the cost of barge-in.

## Camera permission

RIO never takes a picture off its own initiative. The camera is exposed to the
model as a single no-argument tool, `capture_image`, declared in the setup
message; the sensor stays unpowered until a request has been asked aloud and
answered.

```
user: "what am I holding?"
  model -> toolCall capture_image
  fw    -> "permission_required"          camera NOT initialised
  model : "I'd need to use the camera - is that okay?"

user: "yeah, go ahead"
  model -> toolCall capture_image
  fw    -> gate opens: esp_camera_init, capture, esp_camera_deinit
        -> "image_captured"
        -> {"clientContent":{"turns":[{"role":"user","parts":[{"inlineData":
             {"mimeType":"image/jpeg","data":"<b64 jpeg>"}}]}],"turnComplete":true}}
  model : "You're holding a blue mug."
```

The photo is a `clientContent` turn, sent **after** the tool call is answered,
and both halves of that are load-bearing. `realtimeInput.video` is what the Live
API documents for camera input and the server accepts it silently, but
`gemini-3.1-flash-live-preview` never puts those frames in context: the model
answers as though a photo arrived and describes a scene it invented. Opening the
image turn *before* answering the call fails differently — the turn is accepted
and the model then stays silent for the rest of the exchange. Answer first, then
send the photo as its own complete turn.

For a non-visual question the model simply never calls the tool, so none of
this runs and the camera is never touched.

**The gate is enforced in firmware, not by the prompt.** The first call for any
request is refused unconditionally. A later call is honoured only if two things
happened since, in order:

1. the model **finished a turn** (`serverContent.turnComplete`) — it actually
   asked the question, rather than just calling the tool twice in a row; and
2. the user **began a turn** after that (a new `inputTranscription`) — they
   actually said something back.

Both counters live in [gemini_live.cpp](gemini_live.cpp). Requiring the user
turn to follow the model's turn boundary is what stops a late-arriving
transcription fragment from the *original* question being mistaken for an
answer to a question that had not been asked yet. A grant is consumed by one
capture: the next request re-arms the gate from scratch.

**Known limitation.** The firmware guarantees a question was asked and
answered; it does not judge *what* the answer was. Deciding that "no, don't"
is a refusal is left to the model (the system instruction in
[config.h](config.h) tells it not to re-call after a decline). If you want a
consent signal the model cannot misread, the honest version is a physical
button press rather than a spoken yes.

### What a capture costs, and why

Tearing the sensor down after every shot is the whole privacy guarantee, and the
bill for it is that the sensor's automatic loops get no history: auto-exposure,
auto-gain and auto-white-balance start cold every single time, with one frame's
worth of scene to converge on. A blind grab under those conditions comes out
dark, colour-cast, or both.

So a capture is not a grab — it is a short, measured convergence, run by
[camera.cpp](camera.cpp):

| Phase | What it does | Cost |
|---|---|---|
| init | Sensor up at the largest frame size that actually *streams* — each rung of `CAM_FRAME_SIZE` → XGA → SVGA → VGA is proved with a probe frame before it is accepted | ~200–400 ms |
| settle | Decode each frame at 1/8 scale, loop until brightness, **colour** and **stillness** all agree on two consecutive frames | 3–10 frames |
| trim | Bias exposure toward a target a vision model can actually read — the sensor's own AE aims darker | 0–2 frames |
| tone | Classify the scene (high-range / dark / flat) and reshape gamma, contrast, gain ceiling to fit it into 8 bits | free (sensor DSP) |
| bracket | On contrasty or dark scenes, try darker exposures and keep the one that clips the fewest **highlights** | 0–2 small frames |
| focus | Autofocus, if the sensor has a lens that can — neither stock sensor does | 0 ms on OV2640/OV3660 |
| shoot | `CAM_SHOT_CANDIDATES` full-resolution frames, keep the **sharpest**; re-shot at coarser quality if one would not fit `IMG_TX_BYTES` | ~250 ms each |

Everything between init and the shutter is best-effort and bounded by
`CAM_TUNE_BUDGET_MS` (900 ms); the shutter has its own `CAM_SHOT_BUDGET_MS`
(700 ms). When either clock runs out the pipeline shoots with whatever it has
converged on so far: a slightly mis-exposed answer now beats a perfect one after
RIO has already gone quiet waiting. Total is typically 1.5–2 s, against ~0.5 s
for the old blind-warm-up version.

**Two of those phases exist specifically to stop bad frames reaching the model,**
because a bad frame does not fail cheaply — it costs a wrong answer plus the
whole spoken round trip to ask again:

- **Colour convergence.** AWB settles noticeably later than AE, so a frame
  grabbed the moment brightness stops moving is routinely still colour-cast —
  the difference between "the cable is blue" and "the cable is grey". The settle
  loop watches the R/G and B/G channel ratios alongside luma and will not commit
  until both have stopped moving. Free: the ratios come out of thumbnails the
  pipeline is already decoding.
- **Blur rejection, in two layers.** An 8×8 luma grid diffed between consecutive
  thumbnails gives a frame-to-frame motion estimate, and the settle loop refuses
  to commit while the scene is moving — free, same thumbnails. That covers the
  metering window but not the gap between it and the shutter (the resolution
  change, plus autofocus on an OV5640), so the shutter itself takes
  `CAM_SHOT_CANDIDATES` frames and keeps the one with the highest variance of
  the Laplacian at 1/4 scale — the standard focus measure, and a direct read on
  how much fine detail survived. This one is *not* free: ~400 ms for the default
  of 2 candidates. Set `CAM_SHOT_CANDIDATES` to 1 to disable it entirely.

The only per-frame CPU work is decoding thumbnails — 1/8 scale for metering, 1/4
for sharpness scoring. Every actual image adjustment happens inside the sensor's
ISP on the way to its hardware JPEG encoder, so netTask is never blocked doing
arithmetic over a full-resolution image.

#### Why metering runs at full resolution

The obvious optimisation is to meter at VGA and only pay for UXGA on the frame
that gets sent — small frames answer "what exposure?" just as well, far faster.
**It does not work, and it fails expensively.** Recording it here so nobody
re-derives it:

`esp_camera_init()` is the only caller of `cam_config()`, which is what sizes the
DMA descriptor chain and the frame buffers. Calling the sensor's `set_framesize()`
afterwards changes what the *sensor* emits while the driver's DMA layout stays
exactly where init left it. Going down is survivable — fewer bytes than the DMA
expects. Coming back up, frames stop completing altogether, and every
`esp_camera_fb_get()` burns its full ~4 s timeout returning `NULL`. Three of
those in a row is a 12-second hole in the conversation and a `capture_failed`
tool response.

`esp_camera_reconfigure()` is the supported way to change frame size, but it is a
deinit/init underneath: it resets the sensor and discards every exposure and tone
setting the metering just worked out, which defeats the purpose.

So the pipeline picks one resolution at init and stays there. `grabSettled()`
also now bails on the first empty grab rather than working through the rest,
which caps a dead sensor at 4 s instead of 12.

**On "auto HDR".** This is not multi-frame radiance fusion. Merging exposures
would mean decoding two full UXGA frames to RGB888 (1.4 MB each), fusing them,
and re-encoding JPEG *in software* — well over a second of CPU, and it throws
away the hardware encoder that makes this board viable at all. What the pipeline
does instead is the half that actually matters on a fixed-lens 8-bit sensor:
measure the real dynamic range, pick the exposure that clips the fewest
highlights (blown highlights are gone for good; crushed shadows are partly
recoverable, which is why the scoring weights them 3:1), and reshape the sensor's
own tone curve to fit that range. Steps two and three cost no CPU at all.

**Sensor differences are handled at runtime, not assumed.** Every `sensor_t`
setter is called through a null-checking macro, because esp32-camera leaves the
ones a given sensor does not implement as NULL and calling through one is an
instant `StoreProhibited` panic mid-capture. The exposure knob is probed rather
than assumed: the OV3660 honours `set_ae_level`, the OV2640 does not and gets the
DSP brightness offset instead. Autofocus is a driver query. Frame size comes from
what init actually settled on, not from what `config.h` asked for.

### Session length caveat

Google documents audio-only Live sessions as much longer-lived than
audio+video ones (the latter around 2 minutes). This firmware sends stills on
demand rather than streaming video, so it should not be treated as a video
session — but if sending a frame does re-classify the session, expect `goAway`
earlier than `SESSION_RECYCLE_MS` (8 min). That path is already handled:
`goAway` recycles at the next turn boundary and the session-resumption handle
carries the conversation across. Watch `recon=` in the `[STAT]` line.

## Software layout

| File | Role |
|---|---|
| [rio_assistant.ino](rio_assistant.ino) | `setup()`/`loop()`, task creation, bring-up modes, telemetry |
| [config.h](config.h) | Pin map, audio format, buffer sizing, Gemini endpoint/VAD tuning, system prompt |
| [app_state.h](app_state.h)/[.cpp](app_state.cpp) | Shared buffers/queues and the half-duplex `ConvState` machine |
| [audio_in.h](audio_in.h)/[.cpp](audio_in.cpp) | I2S0 PDM mic capture, gain, RMS |
| [audio_out.h](audio_out.h)/[.cpp](audio_out.cpp) | I2S1 standard-mode TX to the MAX98357A |
| [aec.h](aec.h)/[.cpp](aec.cpp) | Mic front end: echo cancellation, noise suppression, local VAD, the double-talk gate, the 24→16 kHz reference decimator and its delay line |
| [camera.h](camera.h)/[.cpp](camera.cpp) | On-demand JPEG capture; powers the sensor up and back down around each shot, and runs the metering / auto-exposure / auto-HDR / autofocus convergence in between |
| [wifi_mgr.h](wifi_mgr.h)/[.cpp](wifi_mgr.cpp) | WiFi connect + reconnect with backoff |
| [ws_client.h](ws_client.h)/[.cpp](ws_client.cpp) | Minimal hand-rolled WebSocket-over-TLS client (existing Arduino libraries cap frame size too low for Gemini's reply chunks) |
| [gemini_live.h](gemini_live.h)/[.cpp](gemini_live.cpp) | Gemini Live session setup, message framing, audio in/out encoding, tool calls and the camera permission gate |
| [b64.h](b64.h)/[.cpp](b64.cpp) | Base64 encode/decode for PCM<->JSON |
| [secrets.h](secrets.h) | WiFi + Gemini API key + pinned root CA (not for version control) |

Three FreeRTOS tasks, pinned to keep audio timing off the WiFi/TLS core:

- `micTask` / `spkTask` — core 1, alongside the Arduino loop.
- `netTask` — core 0, sole owner of the WebSocket, alongside the WiFi/lwIP stack.

## Security note

[secrets.h](secrets.h) currently holds a live WiFi password and Gemini API
key in plaintext, carried over from a prior project where they were already
exposed — its own header comment flags both for rotation. This repo isn't
under git yet; once it is, add `secrets.h` to `.gitignore` before the first
commit.

## References

- Gemini Live API (WebSocket protocol, tool use, video input): https://ai.google.dev/gemini-api/docs/live-api
- ESP32-S3 datasheet: https://documentation.espressif.com/esp32-s3_datasheet_en.pdf
- Seeed XIAO ESP32S3 Sense getting-started/specs: https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/
- XIAO ESP32S3 Sense tutorials/examples repo: https://github.com/Mjrovai/XIAO-ESP32S3-Sense
- OV2640 sensor specs/history: https://www.arducam.com/blog/ov2640/
- ESP32-CAM background (different board, useful for camera-interface comparison): https://matchboxscope.github.io/docs/Hardware/ESP32Cam/
