#include "mp4aac.h"
#include "config.h"

#include <esp_heap_caps.h>
// Via the library root header, not libhelix-aac/aacdec.h directly: Arduino
// discovers a library by its top-level headers, and including the nested one
// leaves src/ off the include path entirely. Only the raw C API below is used.
#include "AACDecoderHelix.h"

// -----------------------------------------------------------------------------
// State. Single-instance by design (see mp4aac.h).
// -----------------------------------------------------------------------------
static HAACDecoder sDec = nullptr;

// Points into the caller's moov buffer rather than copying: the table is up to
// ~100 KB on a long track and there is no reason to hold it twice. musicTask
// keeps that buffer alive for as long as the track is playing.
static const uint8_t* sStsz      = nullptr;   // first entry, big-endian uint32s
static uint32_t       sStszCount = 0;
static uint32_t       sUniform   = 0;         // non-zero if all frames are equal

// Decoder output: helix writes at most AAC_MAX_NSAMPS * AAC_MAX_NCHANS shorts.
static int16_t* sPcm  = nullptr;
// Resampled mono.
//
// Sized for UPsampling, not just downsampling. _96 and above are 44.1 kHz and
// shrink a 1024-sample frame to 558, but _48 is 22.05 kHz (1115 out) and _12 is
// 8 kHz (3073 out) — both grow it. Sizing this to AAC_MAX_NSAMPS would not just
// truncate those, it would leave the phase accumulator short of the frame end
// and underflow the re-base below, desyncing every frame after the first.
#define MP4AAC_OUT_MAX (AAC_MAX_NSAMPS * 4)
static int16_t* sOut  = nullptr;

// Resampler state, carried across frames so the interpolation does not restart
// (and click) at every frame boundary.
static uint32_t sStep      = 0;      // input samples per output sample, Q16
static uint32_t sPhase     = 0;      // Q16 position inside the current block
static int16_t  sPrev      = 0;      // last mono sample of the previous block
static uint16_t sChannels  = 2;

static inline uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/**
 * Take the decoder's internal-RAM buffers.
 *
 * Deliberately NOT held for the lifetime of the program. Internal RAM is the
 * scarce resource on this build: MEASURED on hardware, the JioSaavn TLS
 * handshake drives free internal heap down to a few hundred bytes while the
 * Gemini session is up. The lookup always finishes before a track is opened, so
 * holding these 12 KB across it bought nothing and cost exactly the margin that
 * was missing.
 *
 * Internal rather than PSRAM while they ARE held: both are touched once per
 * sample by the decoder and the resampler, and PSRAM latency on that access
 * pattern is the difference between comfortably real-time and not.
 */
static bool takeBuffers() {
    if (sPcm != nullptr && sOut != nullptr) {
        return true;
    }
    sPcm = (int16_t*)heap_caps_malloc(AAC_MAX_NSAMPS * AAC_MAX_NCHANS * sizeof(int16_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    sOut = (int16_t*)heap_caps_malloc(MP4AAC_OUT_MAX * sizeof(int16_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (sPcm == nullptr || sOut == nullptr) {
        Serial.printf("[MP4] decoder buffers unavailable (%u B internal free)\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    return true;
}

bool mp4aacBegin() {
    // Nothing to pre-allocate: see takeBuffers(). Kept so musicBegin() still has
    // one obvious place to fail early if that ever changes again.
    return true;
}

// -----------------------------------------------------------------------------
// Box walking
//
// Only the path down to the sample tables is walked:
//   moov > trak > mdia > minf > stbl > { stsd > mp4a > esds, stsz }
// Anything else is stepped over by its size field.
// -----------------------------------------------------------------------------
static const uint8_t* findBox(const uint8_t* p, const uint8_t* end,
                              const char* type, uint32_t* outSize) {
    while (p + 8 <= end) {
        uint32_t sz = rd32(p);
        if (sz == 1) {
            // 64-bit size. These do not occur in the files this plays (a track
            // would have to exceed 4 GB), but stepping over one correctly is
            // cheaper than mis-parsing it.
            if (p + 16 > end) return nullptr;
            const uint64_t big = ((uint64_t)rd32(p + 8) << 32) | rd32(p + 12);
            if (big > (uint64_t)(end - p)) return nullptr;
            sz = (uint32_t)big;
        } else if (sz == 0) {
            sz = (uint32_t)(end - p);
        }
        if (sz < 8 || p + sz > end) {
            return nullptr;
        }
        if (memcmp(p + 4, type, 4) == 0) {
            if (outSize != nullptr) *outSize = sz;
            return p;
        }
        p += sz;
    }
    return nullptr;
}

/** Read a length from an MPEG-4 descriptor, which is 7 bits per byte. */
static uint32_t descLen(const uint8_t** p, const uint8_t* end) {
    uint32_t n = 0;
    for (int i = 0; i < 4 && *p < end; i++) {
        const uint8_t b = *(*p)++;
        n = (n << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return n;
}

/**
 * Pull the AudioSpecificConfig out of an esds box.
 *
 * The descriptor lengths are variable-width, so the fixed offsets that look
 * like they work on one file quietly land in the wrong place on another. This
 * walks them properly: ES_Descriptor(0x03) > DecoderConfig(0x04) >
 * DecoderSpecificInfo(0x05).
 */
static bool parseEsds(const uint8_t* esds, uint32_t size,
                      uint8_t* aot, uint32_t* rate, uint16_t* chans) {
    static const uint32_t SR[13] = {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000, 7350
    };

    const uint8_t* p   = esds + 12;      // size(4) + 'esds'(4) + version/flags(4)
    const uint8_t* end = esds + size;

    if (p >= end || *p++ != 0x03) return false;
    descLen(&p, end);
    p += 3;                              // ES_ID(2) + stream priority/flags(1)

    if (p >= end || *p++ != 0x04) return false;
    descLen(&p, end);
    p += 13;                             // objectType, streamType, bufSize, bitrates

    if (p >= end || *p++ != 0x05) return false;
    const uint32_t n = descLen(&p, end);
    if (n < 2 || p + 2 > end) return false;

    const uint16_t cfg = (uint16_t)((p[0] << 8) | p[1]);
    const uint8_t  a   = (cfg >> 11) & 0x1F;
    const uint8_t  sri = (cfg >> 7)  & 0x0F;
    const uint8_t  ch  = (cfg >> 3)  & 0x0F;

    if (sri >= 13 || ch == 0 || ch > 2) {
        return false;
    }

    *aot   = a;
    *rate  = SR[sri];
    *chans = ch;
    return true;
}

bool mp4aacOpen(const uint8_t* moov, size_t len, Mp4AacInfo* out) {
    if (moov == nullptr || out == nullptr || len < 8) {
        return false;
    }
    mp4aacClose();
    if (!takeBuffers()) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const uint8_t* end = moov + len;

    uint32_t sz = 0;
    const uint8_t* trak = findBox(moov + 8, end, "trak", &sz);
    if (trak == nullptr) { Serial.println("[MP4] no trak"); return false; }

    const uint8_t* mdia = findBox(trak + 8, trak + sz, "mdia", &sz);
    if (mdia == nullptr) { Serial.println("[MP4] no mdia"); return false; }

    const uint8_t* minf = findBox(mdia + 8, mdia + sz, "minf", &sz);
    if (minf == nullptr) { Serial.println("[MP4] no minf"); return false; }

    const uint8_t* stbl = findBox(minf + 8, minf + sz, "stbl", &sz);
    if (stbl == nullptr) { Serial.println("[MP4] no stbl"); return false; }

    const uint8_t* stblEnd = stbl + sz;

    // --- Codec configuration -------------------------------------------------
    uint32_t stsdSz = 0;
    const uint8_t* stsd = findBox(stbl + 8, stblEnd, "stsd", &stsdSz);
    if (stsd == nullptr) { Serial.println("[MP4] no stsd"); return false; }

    // stsd: size(4) type(4) version/flags(4) entry_count(4), then the entries.
    uint32_t mp4aSz = 0;
    const uint8_t* mp4a = findBox(stsd + 16, stsd + stsdSz, "mp4a", &mp4aSz);
    if (mp4a == nullptr) { Serial.println("[MP4] not an mp4a track"); return false; }

    // The esds sits inside the mp4a sample entry, past its 28-byte fixed part.
    uint32_t esdsSz = 0;
    const uint8_t* esds = findBox(mp4a + 36, mp4a + mp4aSz, "esds", &esdsSz);
    if (esds == nullptr) { Serial.println("[MP4] no esds"); return false; }

    uint8_t  aot   = 0;
    uint32_t rate  = 0;
    uint16_t chans = 0;
    if (!parseEsds(esds, esdsSz, &aot, &rate, &chans)) {
        Serial.println("[MP4] could not read AudioSpecificConfig");
        return false;
    }
    if (aot != 2) {
        // libhelix has no SBR/PS. Refusing loudly beats emitting half-rate mush.
        Serial.printf("[MP4] audioObjectType %u is not AAC-LC — cannot decode\n",
                      (unsigned)aot);
        return false;
    }

    // --- Frame sizes ---------------------------------------------------------
    uint32_t stszSz = 0;
    const uint8_t* stsz = findBox(stbl + 8, stblEnd, "stsz", &stszSz);
    if (stsz == nullptr || stszSz < 20) { Serial.println("[MP4] no stsz"); return false; }

    const uint32_t uniform = rd32(stsz + 12);
    const uint32_t count   = rd32(stsz + 16);
    if (count == 0) { Serial.println("[MP4] empty stsz"); return false; }

    uint32_t maxFrame = 0;
    if (uniform != 0) {
        maxFrame = uniform;
    } else {
        if ((size_t)stszSz < 20 + (size_t)count * 4) {
            Serial.println("[MP4] stsz truncated");
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t v = rd32(stsz + 20 + i * 4);
            if (v > maxFrame) maxFrame = v;
        }
    }
    if (maxFrame == 0 || maxFrame > MUSIC_MAX_FRAME_BYTES) {
        Serial.printf("[MP4] implausible max frame size %u\n", (unsigned)maxFrame);
        return false;
    }

    sStsz      = (uniform != 0) ? nullptr : (stsz + 20);
    sStszCount = count;
    sUniform   = uniform;

    // --- Decoder -------------------------------------------------------------
    sDec = AACInitDecoder();
    if (sDec == nullptr) {
        Serial.println("[MP4] AACInitDecoder failed");
        return false;
    }

    AACFrameInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.nChans       = chans;
    fi.sampRateCore = (int)rate;
    fi.profile      = AAC_PROFILE_LC;

    // copyLast == 0: take the parameters from fi rather than from a previously
    // seen ADTS header, which is the documented path for MP4 sources.
    if (AACSetRawBlockParams(sDec, 0, &fi) != ERR_AAC_NONE) {
        Serial.println("[MP4] AACSetRawBlockParams rejected the stream");
        mp4aacClose();
        return false;
    }

    sChannels = chans;
    sStep     = (uint32_t)(((uint64_t)rate << 16) / SPK_SAMPLE_RATE);
    if (sStep == 0) {
        Serial.println("[MP4] source rate too low to resample");
        mp4aacClose();
        return false;
    }

    // Worst-case output for one full frame, +1 for the partial step at the end.
    const uint32_t worst = ((uint32_t)AAC_MAX_NSAMPS << 16) / sStep + 1;
    if (worst > MP4AAC_OUT_MAX) {
        Serial.printf("[MP4] %u Hz would emit %u samples per frame, over the %u cap\n",
                      (unsigned)rate, (unsigned)worst, (unsigned)MP4AAC_OUT_MAX);
        mp4aacClose();
        return false;
    }
    sPhase    = 0;
    sPrev     = 0;

    out->sampleRate    = rate;
    out->channels      = chans;
    out->frameCount    = count;
    out->maxFrameBytes = maxFrame;
    out->mdatBytes     = 0;

    Serial.printf("[MP4] AAC-LC %u Hz %u ch, %u frames, max frame %u B, "
                  "resample step %u.%03u\n",
                  (unsigned)rate, (unsigned)chans, (unsigned)count,
                  (unsigned)maxFrame,
                  (unsigned)(sStep >> 16),
                  (unsigned)(((sStep & 0xFFFF) * 1000) >> 16));
    return true;
}

uint32_t mp4aacFrameBytes(uint32_t index) {
    if (index >= sStszCount) {
        return 0;
    }
    if (sUniform != 0) {
        return sUniform;
    }
    return rd32(sStsz + index * 4);
}

int mp4aacDecodeFrame(const uint8_t* in, uint32_t inBytes, const int16_t** outPcm) {
    if (sDec == nullptr || in == nullptr || inBytes == 0 || outPcm == nullptr) {
        return -1;
    }

    // helix advances both of these; it wants the address of a mutable pointer.
    unsigned char* p    = (unsigned char*)in;
    int            left = (int)inBytes;

    const int err = AACDecode(sDec, &p, &left, sPcm);
    if (err != ERR_AAC_NONE) {
        return -1;
    }

    AACFrameInfo fi;
    AACGetLastFrameInfo(sDec, &fi);
    if (fi.outputSamps <= 0) {
        return -1;
    }

    const uint16_t ch     = (fi.nChans > 0) ? (uint16_t)fi.nChans : sChannels;
    const uint32_t frames = (uint32_t)fi.outputSamps / ch;
    if (frames == 0) {
        return -1;
    }

    // --- Downmix to mono, in place ------------------------------------------
    // gPcmOut is mono and so is the AEC reference derived from it, so this has
    // to happen either way. Averaging rather than taking one channel: a hard
    // pan would otherwise drop half the mix.
    if (ch == 2) {
        for (uint32_t i = 0; i < frames; i++) {
            sPcm[i] = (int16_t)(((int32_t)sPcm[2 * i] + sPcm[2 * i + 1]) >> 1);
        }
    }

    // --- Resample to SPK_SAMPLE_RATE ----------------------------------------
    // Linear interpolation with a Q16 phase accumulator. The source is always
    // above 24 kHz here, so this decimates, and decimation without a low-pass
    // folds content above 12 kHz back down. That is a real compromise and it is
    // taken knowingly: a polyphase FIR is several times the CPU, the result is
    // going out of a small speaker under a 0.55 gain, and it lands in the AEC
    // reference either way. Prefer JIOSAAVN_QUALITY "_48" (22.05 kHz) if it ever
    // sounds harsh — that source is already band-limited below the output
    // Nyquist, which makes this path alias-free rather than merely acceptable.
    const int16_t* src = sPcm;
    uint32_t       o   = 0;

    while ((sPhase >> 16) < frames && o < MP4AAC_OUT_MAX) {
        const uint32_t k    = sPhase >> 16;
        const uint32_t frac = sPhase & 0xFFFF;

        const int32_t a = (k == 0) ? sPrev : src[k - 1];
        const int32_t b = src[k];

        sOut[o++] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
        sPhase += sStep;
    }

    // Re-base the phase onto the next block and remember the sample the next
    // block's first interpolation needs. The clamp is belt-and-braces: with
    // MP4AAC_OUT_MAX sized above the worst case the loop always consumes the
    // whole block, and an unsigned wrap here would be silent and fatal.
    const uint32_t consumed = frames << 16;
    sPhase = (sPhase >= consumed) ? (sPhase - consumed) : 0;
    sPrev  = src[frames - 1];

    *outPcm = sOut;
    return (int)o;
}

void mp4aacClose() {
    if (sDec != nullptr) {
        AACFreeDecoder(sDec);
        sDec = nullptr;
    }
    if (sPcm != nullptr) { heap_caps_free(sPcm); sPcm = nullptr; }
    if (sOut != nullptr) { heap_caps_free(sOut); sOut = nullptr; }
    sStsz      = nullptr;
    sStszCount = 0;
    sUniform   = 0;
    sPhase     = 0;
    sPrev      = 0;
}
