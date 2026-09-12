#ifndef MUSIC_H
#define MUSIC_H

#include <Arduino.h>

// =============================================================================
// Song playback.
//
// One task, one job: take a song name, resolve it to a YouTube video id with
// youtube.cpp, then stream raw PCM for that id from the companion helper into
// gPcmOut — the same buffer Gemini's speech goes into, so spkTask, the I2S path
// and the AEC reference all work unchanged.
//
// Why a task and not a call from netTask, which is where every other tool is
// serviced: a search is seconds and a song is minutes. netTask owns the
// WebSocket and must keep polling it, so it hands the request over and returns
// immediately. That is also why the tool answers "searching" rather than
// reporting success — nothing is known yet at the moment the answer is due.
// A failure that emerges later is reported back through musicTakeNotice().
// =============================================================================

/** Create the request queue and start musicTask. Call once from setup(). */
bool musicBegin();

/**
 * Queue a song for playback, replacing anything already queued or playing.
 * Returns false only if the request is unusable or the task is not running.
 * Never blocks.
 */
bool musicRequest(const char* songName);

/** Ask the current playback to stop. Safe from any task; returns immediately. */
void musicStop();

/** True while a lookup or a stream is in progress. */
bool musicIsActive();

/**
 * True only once audio is actually reaching gPcmOut.
 *
 * Distinct from musicIsActive() because a lookup takes seconds, and during
 * those seconds the model is busy saying "playing it now". Treating that
 * confirmation as a barge-in cancels the very song it is confirming, which is
 * exactly what happened before this existed. Barge-in tests THIS; the stop_song
 * tool and musicStop() still act on a lookup in flight.
 */
bool musicIsPlaying();

/**
 * Collect a pending message for the model — always a failure, since success is
 * simply audible. Copies at most `cap` bytes and clears the slot.
 *
 * netTask only: it is the sole owner of the WebSocket that the text goes out on.
 * @return true if a notice was waiting.
 */
bool musicTakeNotice(char* dst, size_t cap);

#endif  // MUSIC_H
