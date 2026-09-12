#ifndef AUDIO_IN_H
#define AUDIO_IN_H

#include <Arduino.h>
#include "driver/i2s_pdm.h"   // ESP-IDF 5.x PDM API — requires Arduino-ESP32 core 3.x

/**
 * Initialise the onboard PDM microphone on I2S_NUM_0.
 * PDM RX is only available on I2S0 on the ESP32-S3, hence the explicit port.
 * @return true if the channel is live and delivering DMA data.
 */
bool audioInBegin();

/**
 * Read up to maxSamples 16-bit mono samples.
 * @return number of samples actually read (may be short on timeout).
 */
size_t audioInRead(int16_t* dst, size_t maxSamples, uint32_t timeoutMs);

/**
 * Discard everything currently sitting in the RX DMA buffers.
 * Called when leaving the muted state so the resumed stream does not begin
 * with audio captured while the speaker was playing.
 */
void audioInDrain();

/** Apply gain with saturation clipping (no integer wraparound). */
void audioApplyGain(int16_t* buf, size_t sampleCount, float gain);

/** Root-mean-square of a sample block — used for level logging / gain tuning. */
float audioRms(const int16_t* buf, size_t sampleCount);

void audioInEnd();

#endif  // AUDIO_IN_H
