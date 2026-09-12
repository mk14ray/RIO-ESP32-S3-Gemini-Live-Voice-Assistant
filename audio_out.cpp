#include "audio_out.h"
#include "config.h"
#include <math.h>

static i2s_chan_handle_t tx_chan = NULL;

// Mono -> stereo staging buffer, internal RAM (DMA-reachable).
// 512 mono samples -> 1024 stereo samples -> 2048 bytes.
//
// File-static rather than on the stack: spkTask has a modest stack and is the
// only runtime caller of the write paths, so a shared buffer is safe here.
#define STAGE_MONO_SAMPLES 512
static int16_t stage[STAGE_MONO_SAMPLES * 2];
static int16_t silence[STAGE_MONO_SAMPLES];

// Amplifier shutdown line. `ampReady` guards the pinMode so the call is cheap
// enough to sit on the write path; `ampOn` mirrors the pin so a no-change
// request costs a comparison rather than a GPIO write plus a settling delay.
static bool ampOn    = false;
static bool ampReady = false;

void audioOutAmpBegin() {
#if AMP_MUTE_ENABLE
    if (ampReady) {
        return;
    }
    pinMode(AMP_SD_PIN, OUTPUT);
    digitalWrite(AMP_SD_PIN, LOW);       // < 0.16 V: shutdown
    ampOn    = false;
    ampReady = true;
    Serial.printf("[SPK] amp mute on GPIO %d (held in shutdown)\n", AMP_SD_PIN);
#endif
}

void audioOutAmpSet(bool on) {
#if AMP_MUTE_ENABLE
    audioOutAmpBegin();
    if (on == ampOn) {
        return;
    }
    digitalWrite(AMP_SD_PIN, on ? HIGH : LOW);
    ampOn = on;
    if (on) {
        // Only on the rising edge, and only ever once per turn: the part is not
        // driving the speaker yet, so samples written now would be clipped.
        delay(AMP_UNMUTE_SETTLE_MS);
    }
#else
    (void)on;
#endif
}

bool audioOutAmpIsOn() {
#if AMP_MUTE_ENABLE
    return ampOn;
#else
    return true;
#endif
}

bool audioOutBegin() {
    if (tx_chan != NULL) {
        return true;
    }

    // Before the channel is enabled, not after. BCLK starting is what makes the
    // amplifier thump, so it has to start into a part that is already shut down.
    audioOutAmpBegin();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.auto_clear    = true;   // emit zeros on underrun rather than repeating the last buffer
    // 4 * 360 = 1440 frames = 60 ms @ 24 kHz.
    //
    // Halved from 120 ms for full duplex. i2s_channel_write blocks until the DMA
    // accepts, so this queue runs near full during playback and its depth IS the
    // dominant term in AEC_REF_DELAY_MS — and the part that swings, since it
    // starts empty at the top of each reply. A shorter queue means a smaller and
    // steadier delay for the canceller to track, and less end-to-end latency.
    // Going much below this risks underruns when spkTask is preempted.
    chan_cfg.dma_desc_num  = 4;
    chan_cfg.dma_frame_num = 360;

    // i2s_new_channel(cfg, tx_handle, rx_handle) — TX is the SECOND argument.
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
    if (err != ESP_OK) {
        Serial.printf("[SPK] i2s_new_channel failed: 0x%X\n", err);
        tx_chan = NULL;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,              // MAX98357A generates its own clock
            .bclk = (gpio_num_t)I2S_SPK_BCLK_PIN,
            .ws   = (gpio_num_t)I2S_SPK_LRC_PIN,
            .dout = (gpio_num_t)I2S_SPK_DIN_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
    if (err != ESP_OK) {
        Serial.printf("[SPK] i2s_channel_init_std_mode failed: 0x%X\n", err);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return false;
    }

    err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK) {
        Serial.printf("[SPK] i2s_channel_enable failed: 0x%X\n", err);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
        return false;
    }

    Serial.printf("[SPK] I2S1 STD TX live @ %d Hz on BCLK=%d WS=%d DOUT=%d\n",
                  SPK_SAMPLE_RATE, I2S_SPK_BCLK_PIN, I2S_SPK_LRC_PIN, I2S_SPK_DIN_PIN);
    return true;
}

size_t audioOutWriteMono(const int16_t* mono, size_t samples, uint32_t timeoutMs) {
    if (mono == nullptr || samples == 0 || tx_chan == NULL) {
        return 0;
    }

    // Safety net, not the primary hook. spkTask unmutes explicitly before it
    // pushes the AEC reference (see spkTask), so on the runtime path this is
    // already a no-op. It exists so that every other writer — the bring-up tone,
    // the loopback, the BRINGUP_AEC noise task — is audible without each one
    // having to remember the amp, which is exactly the kind of omission that
    // makes a bring-up mode look like broken wiring.
    audioOutAmpSet(true);

    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > STAGE_MONO_SAMPLES) {
            n = STAGE_MONO_SAMPLES;
        }

        for (size_t i = 0; i < n; i++) {
            int16_t s = mono[done + i];
            stage[2 * i]     = s;   // left
            stage[2 * i + 1] = s;   // right
        }

        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_chan, stage, n * 2 * sizeof(int16_t),
                                          &written, pdMS_TO_TICKS(timeoutMs));
        if (err != ESP_OK) {
            Serial.printf("[SPK] write failed: 0x%X\n", err);
            break;
        }

        // written is in bytes of stereo data; convert back to mono samples.
        size_t monoWritten = written / (2 * sizeof(int16_t));
        done += monoWritten;

        if (monoWritten < n) {
            break;   // timed out with a partial write
        }
    }

    return done;
}

void audioOutWriteSilence(uint32_t ms) {
    if (tx_chan == NULL || ms == 0) {
        return;
    }

    size_t total = (size_t)((uint64_t)SPK_SAMPLE_RATE * ms / 1000);
    memset(silence, 0, sizeof(silence));

    while (total > 0) {
        size_t n = total > STAGE_MONO_SAMPLES ? STAGE_MONO_SAMPLES : total;
        size_t w = audioOutWriteMono(silence, n, 500);
        if (w == 0) {
            break;
        }
        total -= w;
    }
}

void audioOutTestTone(uint32_t freqHz, uint32_t ms) {
    if (tx_chan == NULL) {
        return;
    }

    const size_t total = (size_t)((uint64_t)SPK_SAMPLE_RATE * ms / 1000);
    int16_t buf[STAGE_MONO_SAMPLES];
    size_t emitted = 0;

    while (emitted < total) {
        size_t n = total - emitted;
        if (n > STAGE_MONO_SAMPLES) {
            n = STAGE_MONO_SAMPLES;
        }

        for (size_t i = 0; i < n; i++) {
            double t = (double)(emitted + i) / (double)SPK_SAMPLE_RATE;
            buf[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * (double)freqHz * t));
        }

        if (audioOutWriteMono(buf, n, 1000) == 0) {
            break;
        }
        emitted += n;
    }
}

void audioOutEnd() {
    if (tx_chan != NULL) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
}
