#ifndef AEC_H
#define AEC_H

#include <Arduino.h>

// =============================================================================
// Microphone front end: echo cancellation -> noise suppression -> echo gate.
//
// The speaker is centimetres from the mic. With the mic open during playback
// (which is what full duplex means) it hears RIO far louder than the user, and
// unless that is removed Gemini's VAD reads it as a barge-in: RIO interrupts
// itself, its own sentences come back as user transcripts, and in the worst
// case the model stops taking turns at all because it believes the user never
// stops talking.
//
// Three stages, in order:
//
//   1. AEC   subtracts the known playback signal. Effective only when
//            AEC_REF_DELAY_MS is calibrated (BRINGUP_AEC).
//   2. NS    suppresses steady background noise, which both improves what
//            Gemini hears and stops a high noise floor from holding the VAD
//            open permanently.
//   3. GATE  decides whether what survived is really the user, or just echo
//            the AEC failed to remove.
//
// Stage 3 is what makes this robust rather than merely good when tuned. It
// compares the residual against the echo level *predicted from the measured
// ERLE*, so it adapts to how well cancellation is actually working:
//
//   ERLE high  -> predicted residual is tiny -> user speech clears it easily,
//                 barge-in works, conversation is genuinely full duplex.
//   ERLE ~0 dB -> predicted residual is the whole reference -> nothing clears
//                 it while RIO talks, so the mic is effectively muted for the
//                 duration and the system degrades to half duplex.
//
// The failure mode is therefore "loses barge-in", never "talks to itself".
// That property is the point: an uncalibrated or drifting delay costs a
// feature instead of breaking the conversation.
//
// Not thread-safe. aecPushReference() is called by spkTask, aecProcess() by
// micTask; see the note on the delay line in aec.cpp for why that needs no lock.
// =============================================================================

/** Outcome of one processed frame — diagnostics and the gate decision. */
struct AecStatus {
    bool  speech;    // local VAD found speech in this frame
    bool  gated;     // suppressed: judged to be RIO's own voice
    bool  playing;   // the speaker was driving the room during this frame
    float micRms;    // level in, before cancellation
    float outRms;    // level out, after cancellation and suppression
};

/** Allocate the canceller, the suppressor, the detector and the delay line. */
bool aecBegin();

/** Frame size the canceller consumes, in 16 kHz samples. Valid after aecBegin(). */
size_t aecFrameSamples();

/** Largest number of samples one aecProcess() call can emit. */
size_t aecMaxOutputSamples();

/**
 * Record what is being sent to the speaker, at SPK_SAMPLE_RATE.
 * Called by spkTask immediately before the I2S write, so the delay line sees
 * exactly the samples the amplifier does, in the same order.
 */
void aecPushReference(const int16_t* spk, size_t samples);

/** Drop the pending reference — the audio it describes is never going to play. */
void aecFlushReference();

/**
 * Run one mic frame through the chain.
 *
 * `mic` is exactly aecFrameSamples() long. `out` must hold at least
 * aecMaxOutputSamples(); the output is a whole number of noise-suppressor
 * frames and so does not always match the input length — the remainder is
 * carried to the next call rather than dropped.
 *
 * Gated frames are emitted as true silence, not attenuated audio: the point is
 * to give Gemini's VAD nothing whatsoever to trigger on.
 *
 * @return samples written to `out`.
 */
size_t aecProcess(const int16_t* mic, int16_t* out, AecStatus* status);

/** Override the reference delay at runtime, in ms. Used by the BRINGUP_AEC sweep. */
void aecSetRefDelayMs(uint32_t ms);

/** Forget the smoothed ERLE. Call after changing the delay, before measuring. */
void aecResetErle();

/**
 * Echo return loss enhancement in dB, averaged over recent frames where the
 * speaker was active — how much of RIO's own voice is being removed. Above
 * ~20 dB is healthy. Near 0 means the reference is misaligned, and the gate
 * will hold the mic shut whenever RIO speaks.
 */
float aecErleDb();

/** Frames suppressed by the gate since boot — how often RIO caught itself. */
uint32_t aecGatedFrames();

#endif  // AEC_H
