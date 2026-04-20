#define _GNU_SOURCE
#include "heap_smith/allocator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 * Per-thread cache
 *
 * Each thread keeps a small magazine of recently freed blocks for each
 * size class.  On free, a block is stashed in the cache instead of being
 * returned to the global free list.  On alloc, the cache is checked first.
 * This eliminates lock contention for the hot path in multi-threaded code.
 * ====================================================================== */

#define NUM_SIZE_CLASSES   10
#define TC_MAX_PER_CLASS   16

static const size_t g_tc_sizes[NUM_SIZE_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};

typedef struct {
    void  *blocks[TC_MAX_PER_CLASS];
    int    count;
} TcBin;

typedef struct {
    TcBin bins[NUM_SIZE_CLASSES];
    int   initialised;
} ThreadCache;

static __thread ThreadCache tc = { .initialised = 0 };

static void tc_init(void)
{
    memset(&tc, 0, sizeof(tc));
    tc.initialised = 1;
}

static int tc_size_index(size_t size)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (size <= g_tc_sizes[i]) return i;
    }
    return -1;
}

/**
 * tc_alloc - try to satisfy an allocation from the thread cache.
 * Returns a pointer if the cache has a block of the right size class,
 * NULL otherwise (caller should fall back to hs_malloc).
 */
void *tc_alloc(size_t size)
{
    if (!tc.initialised) tc_init();
    int idx = tc_size_index(size);
    if (idx < 0) return NULL;
    TcBin *bin = &tc.bins[idx];
    if (bin->count == 0) return NULL;
    return bin->blocks[--bin->count];
}

/**
 * tc_free - try to cache a freed block.
 * Returns 1 if the block was stashed (caller should not free it further),
 * 0 if the cache is full (caller should return the block to global lists).
 */
int tc_free(void *ptr, size_t size)
{
    if (!tc.initialised) tc_init();
    int idx = tc_size_index(size);
    if (idx < 0) return 0;
    TcBin *bin = &tc.bins[idx];
    if (bin->count >= TC_MAX_PER_CLASS) return 0;
    bin->blocks[bin->count++] = ptr;
    return 1;
}
