#ifndef APP_STATE_H
#define APP_STATE_H

#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"

// =============================================================================
// Shared state and buffers for the half-duplex conversation loop.
//
// Because the speaker sits next to the mic with no echo cancellation, the mic
// is muted for the whole reply plus a tail. The state machine below coordinates
// that between three tasks without a mutex:
//
//   netTask owns   LISTENING -> SPEAKING
//   spkTask owns   SPEAKING  -> TAIL -> LISTENING
//   micTask is a pure reader
//
// The two writers own disjoint transitions and each uses compare_exchange from
// the state it owns, so a lost update is impossible.
// =============================================================================

enum ConvState : uint8_t {
    CONV_BOOT = 0,
    CONV_CONNECTING,
    CONV_LISTENING,
    CONV_SPEAKING,
    CONV_TAIL,
};

extern std::atomic<uint8_t> gConv;
extern std::atomic<bool>    gTurnComplete;   // set by netTask, cleared by spkTask
extern std::atomic<bool>    gFlushPlayback;  // set by netTask on `interrupted`

// Decoded reply PCM: netTask (writer) -> spkTask (reader).
extern StreamBufferHandle_t gPcmOut;

// Outbound JSON frames: micTask fills a slot, netTask sends it.
// Index queues rather than copying queues, so 4.4 KB payloads are never copied.
extern QueueHandle_t gTxReady;   // uint8_t slot indices ready to send
extern QueueHandle_t gTxFree;    // uint8_t slot indices available to fill
extern uint8_t*      gTxSlots;   // TX_SLOT_COUNT * TX_SLOT_BYTES, PSRAM
extern size_t*       gTxSlotLen; // byte length currently held in each slot

// Reassembly buffer owned by WsClient.
extern uint8_t* gWsRxBuf;

// One fully-built image realtimeInput message. Far too big for a TX slot, and
// only ever built and sent by netTask, so it needs no queue.
extern char* gImgTxBuf;

// Diagnostics
extern std::atomic<uint32_t> gDroppedChunks;
extern std::atomic<uint32_t> gOverflows;
extern std::atomic<uint32_t> gReconnects;
// gPcmOut ran dry with a reply still in flight — i.e. the speaker caught up
// with the network. Every count is a break the listener can hear, so this is
// the number to watch when tuning PLAYBACK_PREBUFFER_MS.
extern std::atomic<uint32_t> gUnderruns;

/** Allocate every large buffer. Call once in setup(), before any task starts. */
bool appStateBegin();

const char* convStateName(uint8_t s);

#endif  // APP_STATE_H
