#include "memmon.h"
#include "config.h"

#include <esp_heap_caps.h>

size_t memFreeInternal() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t memLargestInternal() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

size_t memMinEverInternal() {
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool memHaveInternal(size_t need, const char* what) {
    const size_t freeNow = memFreeInternal();
    const size_t largest = memLargestInternal();

    // Both tests matter and they fail differently. `freeNow` catches "the heap
    // is genuinely almost gone"; `largest` catches "there is plenty free but it
    // is all in pieces too small for one TLS record buffer". Requiring the
    // largest block to cover half the need is a heuristic, not a proof: mbedtls
    // takes its memory in several chunks rather than one, so demanding the full
    // amount contiguously would refuse operations that would have succeeded.
    if (freeNow >= need && largest >= need / 2) {
        return true;
    }

    Serial.printf("[MEM] declining %s: need ~%u B, have %u B free "
                  "(largest block %u B)\n",
                  what, (unsigned)need, (unsigned)freeNow, (unsigned)largest);
    return false;
}

void memLog(const char* tag) {
    Serial.printf("[MEM] %-14s internal free=%u largest=%u minEver=%u | "
                  "psram free=%u largest=%u\n",
                  tag,
                  (unsigned)memFreeInternal(),
                  (unsigned)memLargestInternal(),
                  (unsigned)memMinEverInternal(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
