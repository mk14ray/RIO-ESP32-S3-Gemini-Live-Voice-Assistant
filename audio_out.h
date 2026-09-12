#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <Arduino.h>
#include "driver/i2s_std.h"

/**
 * Initialise the MAX98357A on I2S_NUM_1 in standard (Philips) TX mode at
 * SPK_SAMPLE_RATE. I2S1 is used because the microphone owns I2S0 (PDM RX is
 * I2S0-only on the ESP32-S3).
 */
bool audioOutBegin();

/**
 * Claim the MAX98357A SD line and hold the amplifier in shutdown.
 *
 * Idempotent, and deliberately safe to call before audioOutBegin(): call it as
 * early in setup() as possible. AMP_SD_PIN is U0RXD, whose level before the
 * firmware drives it is not guaranteed, so the amplifier is uncontrolled for the
 * whole boot window. Claiming the pin first is what keeps the I2S start-up thump
 * inaudible — audioOutBegin() calls this before it enables the channel.
 *
 * No-op when AMP_MUTE_ENABLE is 0.
 */
void audioOutAmpBegin();

/**
 * Take the amplifier out of shutdown (true) or put it back in (false).
 *
 * Idempotent: a call that does not change the level returns immediately, so the
 * write path can call it per chunk without ever toggling the pin. Only a rising
 * edge blocks, for AMP_UNMUTE_SETTLE_MS.
 *
 * Muting is the caller's judgement and must not be issued with audio still in
 * flight — the TX DMA holds ~60 ms that has been accepted but not yet played.
 * See spkAmpIdleCheck() for the conditions that make it safe.
 */
void audioOutAmpSet(bool on);

/** True while the amplifier is out of shutdown. Always true if AMP_MUTE_ENABLE is 0. */
bool audioOutAmpIsOn();

/**
 * Write mono samples, duplicating each into both stereo slots.
 *
 * The duplication is deliberate. ESP-IDF's STD TX mono mode zero-fills the
 * unselected slot, and a MAX98357A with SD floating averages (L+R)/2 — which
 * would silently halve the output. Sending real stereo costs a copy loop and
 * 2x DMA bandwidth, and removes the ambiguity entirely.
 *
 * Blocks until the DMA has accepted everything (or the timeout expires).
 * @return samples accepted.
 */
size_t audioOutWriteMono(const int16_t* mono, size_t samples, uint32_t timeoutMs);

/**
 * Write `ms` of digital silence.
 *
 * Used for the half-duplex tail: because i2s_channel_write blocks until the DMA
 * has room, this both flushes the last real samples through the amplifier and
 * provides the acoustic tail. Sleeping instead would leave up to a DMA buffer's
 * worth of unplayed audio and the mic would reopen into the end of the reply.
 */
void audioOutWriteSilence(uint32_t ms);

/** Emit a test tone. Step 1 of bring-up: proves wiring and I2S1 in isolation. */
void audioOutTestTone(uint32_t freqHz, uint32_t ms);

void audioOutEnd();

#endif  // AUDIO_OUT_H
