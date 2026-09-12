#ifndef GEMINI_LIVE_H
#define GEMINI_LIVE_H

#include <Arduino.h>

/** One-time init: wires the WebSocket client to the reassembly buffer. */
bool geminiBegin();

/** TLS + WebSocket connect, send setup, wait for setupComplete. */
bool geminiConnect();

void geminiDisconnect();
bool geminiIsConnected();

/** Pump the socket; fires the internal message handler. */
bool geminiPoll(uint32_t maxMs);

/** Send a pre-built JSON frame (produced by geminiBuildAudioMessage). */
bool geminiSendRaw(const char* json, size_t len);

/**
 * Build `{"realtimeInput":{"audio":{"data":"<b64>","mimeType":"audio/pcm;rate=16000"}}}`
 * into dst. Returns false if dst is too small.
 */
bool geminiBuildAudioMessage(const int16_t* pcm, size_t samples,
                             char* dst, size_t cap, size_t* outLen);

/**
 * Act on a tool call received during the last poll, if any.
 *
 * Kept out of the receive callback because a granted capture powers up the
 * camera and blocks for a few hundred milliseconds; netTask should decide when
 * to absorb that, not the WebSocket parser.
 *
 * Must be called from netTask only, and only between polls.
 * @return true if a call was serviced.
 */
bool geminiServicePendingToolCall();

/**
 * Forward one pending music failure to the model, if any is waiting.
 *
 * Success is simply audible, so only failures ever come through here. Sent as
 * a user turn with turnComplete so the model actually speaks it.
 *
 * Must be called from netTask only, and only between polls.
 * @return true if a notice was sent.
 */
bool geminiPumpMusicNotices();

/**
 * Forward one pending news result (success or failure) to the model, if any
 * is waiting. Unlike music, a successful lookup also has to be spoken, so this
 * fires on both outcomes.
 *
 * Must be called from netTask only, and only between polls.
 * @return true if a notice was sent.
 */
bool geminiPumpNewsNotices();

/**
 * Forward one pending holiday result (success or failure) to the model, if
 * any is waiting. Like news, a successful lookup also has to be spoken, so
 * this fires on both outcomes.
 *
 * Must be called from netTask only, and only between polls.
 * @return true if a notice was sent.
 */
bool geminiPumpHolidayNotices();

/** True once the server has asked us to reconnect, or the session is stale. */
bool geminiShouldRecycle();

#endif  // GEMINI_LIVE_H
