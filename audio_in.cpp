#include "audio_in.h"
#include "config.h"
#include <math.h>

static i2s_chan_handle_t rx_chan = NULL;

bool audioInBegin() {
    if (rx_chan != NULL) {
        return true;
    }

    // I2S_NUM_0 explicitly: PDM RX is not available on I2S1 on the ESP32-S3.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear   = true;
    // 6 * 256 = 1536 frames = 96 ms @ 16 kHz.
    //
    // Down from 200 ms. This is a ceiling rather than a fixed cost — micTask
    // keeps up, so the queue normally holds only a descriptor or two — but it
    // sets how far the capture path can drift behind real time when micTask
    // stalls (it can block up to 50 ms on a full TX slot queue), and that drift
    // lands directly on the AEC's delay. 96 ms keeps ample margin over that
    // worst case while capping the drift.
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 256;

    // i2s_new_channel(cfg, tx_handle, rx_handle) — RX is the THIRD argument.
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx_chan);
    if (err != ESP_OK) {
        Serial.printf("[MIC] i2s_new_channel failed: 0x%X\n", err);
        rx_chan = NULL;
        return false;
    }

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                   I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = (gpio_num_t)I2S_MIC_CLK_PIN,
            .din = (gpio_num_t)I2S_MIC_DATA_PIN,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };

    err = i2s_channel_init_pdm_rx_mode(rx_chan, &pdm_rx_cfg);
    if (err != ESP_OK) {
        Serial.printf("[MIC] i2s_channel_init_pdm_rx_mode failed: 0x%X\n", err);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return false;
    }

    err = i2s_channel_enable(rx_chan);
    if (err != ESP_OK) {
        Serial.printf("[MIC] i2s_channel_enable failed: 0x%X\n", err);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return false;
    }

    delay(200);   // PDM mic needs ~100 ms after the clock starts

    // Sanity check: prove DMA is actually delivering before we build on top of it.
    int16_t testBuf[64] = {0};
    size_t testBytes = 0;
    err = i2s_channel_read(rx_chan, testBuf, sizeof(testBuf), &testBytes, pdMS_TO_TICKS(500));
    if (err != ESP_OK || testBytes == 0) {
        Serial.printf("[MIC] post-init read failed (err=0x%X, bytes=%u). Check GPIO %d/%d.\n",
                      err, (unsigned)testBytes, I2S_MIC_CLK_PIN, I2S_MIC_DATA_PIN);
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
        return false;
    }

    Serial.printf("[MIC] I2S0 PDM RX live @ %d Hz (%u bytes in test read)\n",
                  MIC_SAMPLE_RATE, (unsigned)testBytes);
    return true;
}

size_t audioInRead(int16_t* dst, size_t maxSamples, uint32_t timeoutMs) {
    if (dst == nullptr || maxSamples == 0 || rx_chan == NULL) {
        return 0;
    }

    size_t bytesRead = 0;
    esp_err_t err = i2s_channel_read(rx_chan, dst, maxSamples * sizeof(int16_t),
                                     &bytesRead, pdMS_TO_TICKS(timeoutMs));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        Serial.printf("[MIC] read failed: 0x%X\n", err);
        return 0;
    }

    return bytesRead / sizeof(int16_t);
}

void audioInDrain() {
    if (rx_chan == NULL) {
        return;
    }

    int16_t scratch[256];
    size_t bytesRead = 0;
    // Zero timeout: pull only what is already buffered, then stop.
    while (i2s_channel_read(rx_chan, scratch, sizeof(scratch), &bytesRead, 0) == ESP_OK
           && bytesRead > 0) {
        bytesRead = 0;
    }
}

void audioApplyGain(int16_t* buf, size_t sampleCount, float gain) {
    if (buf == nullptr || gain == 1.0f) {
        return;
    }

    for (size_t i = 0; i < sampleCount; i++) {
        int32_t v = (int32_t)((float)buf[i] * gain);
        if (v > 32767)       v = 32767;
        else if (v < -32768) v = -32768;
        buf[i] = (int16_t)v;
    }
}

float audioRms(const int16_t* buf, size_t sampleCount) {
    if (buf == nullptr || sampleCount == 0) {
        return 0.0f;
    }

    double acc = 0.0;
    for (size_t i = 0; i < sampleCount; i++) {
        double s = (double)buf[i];
        acc += s * s;
    }
    return (float)sqrt(acc / (double)sampleCount);
}

void audioInEnd() {
    if (rx_chan != NULL) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }
}
