#include "app_state.h"
#include "config.h"
#include <esp_heap_caps.h>

std::atomic<uint8_t> gConv(CONV_BOOT);
std::atomic<bool>    gTurnComplete(false);
std::atomic<bool>    gFlushPlayback(false);

StreamBufferHandle_t gPcmOut = NULL;

QueueHandle_t gTxReady   = NULL;
QueueHandle_t gTxFree    = NULL;
uint8_t*      gTxSlots   = nullptr;
size_t*       gTxSlotLen = nullptr;

uint8_t* gWsRxBuf  = nullptr;
char*    gImgTxBuf = nullptr;

std::atomic<uint32_t> gDroppedChunks(0);
std::atomic<uint32_t> gOverflows(0);
std::atomic<uint32_t> gReconnects(0);
std::atomic<uint32_t> gUnderruns(0);

// Static control block for the stream buffer — lives in internal RAM while the
// storage itself lives in PSRAM.
static StaticStreamBuffer_t sPcmCtrl;

const char* convStateName(uint8_t s) {
    switch (s) {
        case CONV_BOOT:       return "BOOT";
        case CONV_CONNECTING: return "CONNECTING";
        case CONV_LISTENING:  return "LISTENING";
        case CONV_SPEAKING:   return "SPEAKING";
        case CONV_TAIL:       return "TAIL";
        default:              return "?";
    }
}

static void* psAlloc(size_t bytes, const char* what) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (p == nullptr) {
        Serial.printf("[MEM] FAILED to allocate %u bytes of PSRAM for %s\n",
                      (unsigned)bytes, what);
    } else {
        Serial.printf("[MEM] %-12s %7u bytes PSRAM\n", what, (unsigned)bytes);
    }
    return p;
}

bool appStateBegin() {
    Serial.printf("[MEM] PSRAM available: %u bytes\n", (unsigned)ESP.getFreePsram());

    if (ESP.getFreePsram() == 0) {
        Serial.println("[MEM] No PSRAM detected! Set Tools > PSRAM to \"OPI PSRAM\".");
        return false;
    }

    // --- Reply PCM stream ----------------------------------------------------
    // NOTE the mandatory +1: xStreamBufferCreateStatic requires storage of
    // (size + 1) bytes. Getting this wrong silently corrupts the buffer.
    uint8_t* pcmStore = (uint8_t*)psAlloc(PCM_STREAM_BYTES + 1, "pcm stream");
    if (pcmStore == nullptr) {
        return false;
    }
    gPcmOut = xStreamBufferCreateStatic(PCM_STREAM_BYTES, 1, pcmStore, &sPcmCtrl);
    if (gPcmOut == NULL) {
        Serial.println("[MEM] xStreamBufferCreateStatic failed");
        return false;
    }

    // --- Outbound slot pool --------------------------------------------------
    gTxSlots = (uint8_t*)psAlloc((size_t)TX_SLOT_COUNT * TX_SLOT_BYTES, "tx slots");
    if (gTxSlots == nullptr) {
        return false;
    }

    gTxSlotLen = (size_t*)psAlloc(sizeof(size_t) * TX_SLOT_COUNT, "tx slot len");
    if (gTxSlotLen == nullptr) {
        return false;
    }
    memset(gTxSlotLen, 0, sizeof(size_t) * TX_SLOT_COUNT);

    gTxFree  = xQueueCreate(TX_SLOT_COUNT, sizeof(uint8_t));
    gTxReady = xQueueCreate(TX_SLOT_COUNT, sizeof(uint8_t));
    if (gTxFree == NULL || gTxReady == NULL) {
        Serial.println("[MEM] xQueueCreate failed");
        return false;
    }
    for (uint8_t i = 0; i < TX_SLOT_COUNT; i++) {
        xQueueSend(gTxFree, &i, 0);
    }

    // --- WebSocket reassembly ------------------------------------------------
    gWsRxBuf = (uint8_t*)psAlloc(WS_RX_ASSEMBLY_BYTES, "ws rx");
    if (gWsRxBuf == nullptr) {
        return false;
    }

    // --- Image upload staging ------------------------------------------------
    // Allocated once up front rather than around each capture: the camera
    // driver's own framebuffer allocation is the one that has to succeed at
    // capture time — two UXGA buffers, and they are large — and it should not be
    // competing with a half-megabyte request against a PSRAM heap fragmented by
    // hours of uptime.
    gImgTxBuf = (char*)psAlloc(IMG_TX_BYTES, "img tx");
    if (gImgTxBuf == nullptr) {
        return false;
    }

    Serial.printf("[MEM] PSRAM remaining: %u bytes\n", (unsigned)ESP.getFreePsram());
    return true;
}
