#ifndef MP4AAC_H
#define MP4AAC_H

#include <Arduino.h>

// =============================================================================
// MP4 demux + AAC-LC decode + resample to the speaker's rate.
//
// This is the work the companion helper used to do with ffmpeg. It moved on
// device when the source moved from YouTube to JioSaavn, because JioSaavn hands
// out a plain file over plain HTTP and the only thing standing between that
// file and gPcmOut is a container and a codec the S3 can handle itself.
//
// WHAT MAKES THIS TRACTABLE, all MEASURED on aac.saavncdn.com:
//
//   * Every bitrate is plain AAC-LC (audioObjectType 2). No SBR, no PS, so
//     libhelix decodes it without the HE-AAC paths it does not have.
//   * `moov` sits BEFORE `mdat` — 63,283 bytes at offset 28 on a 6-minute
//     track. So the tables can be read and parsed in one forward pass with no
//     seeking, which matters on a socket that cannot seek at all.
//   * The AAC frames fill `mdat` contiguously: sum(stsz) == mdat payload,
//     exactly, and the first chunk offset equals the start of that payload.
//     That is why nothing here reads `stsc` or `stco` — with no gaps to skip,
//     the frame sizes alone are enough to walk the whole track.
//
// MP4 stores raw AAC blocks with no ADTS headers, so there is no sync word to
// hunt for and AACFindSyncWord() is useless here. Frame boundaries come from
// `stsz` and the decoder is put in raw mode with AACSetRawBlockParams().
//
// NOT thread-safe, and single-instance: one decoder, one frame table, one set
// of resampler state. Only musicTask ever calls it.
// =============================================================================

typedef struct {
    uint32_t sampleRate;      // 44100 at _96/_160/_320, 22050 at _48
    uint16_t channels;        // 2 for every quality except _12
    uint32_t frameCount;      // number of AAC frames == stsz entry count
    uint32_t maxFrameBytes;   // largest stsz entry; bounds the read buffer
    uint64_t mdatBytes;       // payload size, for sanity checks only
} Mp4AacInfo;

/** Allocate the decoder and its buffers. Call once, from musicBegin(). */
bool mp4aacBegin();

/**
 * Parse a buffered `moov` box and start the decoder.
 *
 * @param moov  the complete moov box, starting at its 4-byte size field.
 * @param len   bytes available in `moov`.
 * @param out   receives the stream description.
 * @return false if the box is malformed, is not AAC-LC, or has no frame table.
 */
bool mp4aacOpen(const uint8_t* moov, size_t len, Mp4AacInfo* out);

/** Byte length of frame `index`, straight out of the stsz table. */
uint32_t mp4aacFrameBytes(uint32_t index);

/**
 * Decode one raw AAC frame and resample it to SPK_SAMPLE_RATE mono.
 *
 * @param in       the frame, exactly mp4aacFrameBytes(i) long.
 * @param inBytes  its length.
 * @param outPcm   receives a pointer to internal PCM. Valid until the next call.
 * @return number of mono samples produced, or -1 on a decode error.
 */
int mp4aacDecodeFrame(const uint8_t* in, uint32_t inBytes, const int16_t** outPcm);

/** Release the decoder's per-track state. Safe to call when not open. */
void mp4aacClose();

#endif  // MP4AAC_H
