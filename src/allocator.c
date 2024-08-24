#define _GNU_SOURCE
#include "heap_smith/allocator.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* =========================================================================
 * Block header layout (8 bytes)
 *
 *  63 .......... 2 | 1          | 0
 *  size (62 bits)  | prev_in_use| in_use
 *
 * For free blocks the 8 bytes immediately after the header store the
 * free-list next/prev pointers (explicit doubly-linked free list).
 *
 * Minimum allocation is 16 bytes (header + 8 bytes payload), which is
 * enough to store two pointers for the free list.
 * ====================================================================== */

typedef struct BlockHeader {
    size_t  size_flags;     /* size | prev_in_use<<1 | in_use */
} BlockHeader;

#define BH_IN_USE       ((size_t)1)
#define BH_PREV_IN_USE  ((size_t)2)
#define BH_SIZE_MASK    (~(size_t)3)

static inline size_t bh_size(const BlockHeader *h) {
    return h->size_flags & BH_SIZE_MASK;
}
static inline int bh_in_use(const BlockHeader *h) {
    return (int)(h->size_flags & BH_IN_USE);
}
static inline int bh_prev_in_use(const BlockHeader *h) {
    return (int)(h->size_flags & BH_PREV_IN_USE);
}
static inline void bh_set(BlockHeader *h, size_t sz, int in_use, int prev_in_use) {
    h->size_flags = (sz & BH_SIZE_MASK)
                  | (in_use       ? BH_IN_USE      : 0)
                  | (prev_in_use  ? BH_PREV_IN_USE : 0);
}

/* Footer (size field only) placed at end of free block for O(1) coalesce */
typedef struct BlockFooter {
    size_t size;
} BlockFooter;

#define HEADER_SIZE  sizeof(BlockHeader)
#define FOOTER_SIZE  sizeof(BlockFooter)
#define MIN_BLOCK    (HEADER_SIZE + 2 * sizeof(void *))  /* header + 2 ptrs */
#define ALIGN        (sizeof(void *) * 2)                /* 16-byte alignment */

static inline size_t round_up(size_t n, size_t a) {
    return (n + a - 1) & ~(a - 1);
}

/* Pointer arithmetic helpers */
static inline BlockHeader *payload_to_header(void *p) {
    return (BlockHeader *)((uint8_t *)p - HEADER_SIZE);
}
static inline void *header_to_payload(BlockHeader *h) {
    return (void *)((uint8_t *)h + HEADER_SIZE);
}
static inline BlockHeader *next_header(BlockHeader *h) {
    return (BlockHeader *)((uint8_t *)h + bh_size(h));
}
static inline BlockHeader *prev_header(BlockHeader *h) {
    /* Prev footer lives immediately before this header */
    BlockFooter *f = (BlockFooter *)((uint8_t *)h - FOOTER_SIZE);
    return (BlockHeader *)((uint8_t *)h - f->size);
}
static inline BlockFooter *block_footer(BlockHeader *h) {
    return (BlockFooter *)((uint8_t *)h + bh_size(h) - FOOTER_SIZE);
}

/* Free list node (stored in payload area of free blocks) */
typedef struct FreeNode {
    struct FreeNode *prev;
    struct FreeNode *next;
} FreeNode;

static inline FreeNode *header_to_node(BlockHeader *h) {
    return (FreeNode *)header_to_payload(h);
}
static inline BlockHeader *node_to_header(FreeNode *n) {
    return payload_to_header((void *)n);
}

/* =========================================================================
 * Size classes
 * ====================================================================== */

#define NUM_SIZE_CLASSES 10
static const size_t g_size_classes[NUM_SIZE_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};
#define LARGE_THRESHOLD 4096

typedef struct SizeClass {
    FreeNode       *head;           /* explicit free list */
    size_t          block_size;
    size_t          total_blocks;   /* ever allocated */
    size_t          free_blocks;    /* currently free */
    pthread_mutex_t lock;
} SizeClass;

static SizeClass g_classes[NUM_SIZE_CLASSES];
static pthread_mutex_t g_large_lock = PTHREAD_MUTEX_INITIALIZER;

/* =========================================================================
 * Arena (sbrk-based for small allocations)
 * ====================================================================== */

#define ARENA_CHUNK   (256 * 1024)   /* 256 KiB per sbrk expansion */

static pthread_mutex_t g_arena_lock = PTHREAD_MUTEX_INITIALIZER;
static void   *g_arena_start = NULL;
static void   *g_arena_top   = NULL;    /* next free byte */
static void   *g_arena_end   = NULL;    /* one past last byte */
static size_t  g_arena_total = 0;

static void *arena_expand(size_t bytes)
{
    pthread_mutex_lock(&g_arena_lock);
    /* Round up to page size */
    size_t pages = round_up(bytes, (size_t)getpagesize());
    void *p = sbrk((intptr_t)pages);
    if (p == (void *)-1) {
        pthread_mutex_unlock(&g_arena_lock);
        return NULL;
    }
    if (g_arena_start == NULL) g_arena_start = p;
    g_arena_end   = (uint8_t *)p + pages;
    g_arena_top   = p;
    g_arena_total += pages;
    pthread_mutex_unlock(&g_arena_lock);
    return p;
}

/* Carve @bytes from the arena.  Returns NULL if insufficient space. */
static void *arena_bump(size_t bytes)
{
    pthread_mutex_lock(&g_arena_lock);
    if (g_arena_top == NULL || (uint8_t *)g_arena_top + bytes > (uint8_t *)g_arena_end) {
        pthread_mutex_unlock(&g_arena_lock);
        size_t chunk = bytes > ARENA_CHUNK ? bytes : ARENA_CHUNK;
        if (arena_expand(chunk) == NULL) return NULL;
        pthread_mutex_lock(&g_arena_lock);
    }
    void *p       = g_arena_top;
    g_arena_top   = (uint8_t *)g_arena_top + bytes;
    pthread_mutex_unlock(&g_arena_lock);
    return p;
}

/* =========================================================================
 * Statistics
 * ====================================================================== */

static pthread_mutex_t g_stats_lock    = PTHREAD_MUTEX_INITIALIZER;
static size_t          g_total_allocated = 0;
static size_t          g_total_freed     = 0;
static size_t          g_current_usage   = 0;
static size_t          g_peak_usage      = 0;
static uint64_t        g_alloc_count     = 0;
static uint64_t        g_free_count      = 0;

static void stats_alloc(size_t n) {
    pthread_mutex_lock(&g_stats_lock);
    g_total_allocated += n;
    g_current_usage   += n;
    g_alloc_count++;
    if (g_current_usage > g_peak_usage) g_peak_usage = g_current_usage;
    pthread_mutex_unlock(&g_stats_lock);
}
static void stats_free(size_t n) {
    pthread_mutex_lock(&g_stats_lock);
    g_total_freed   += n;
    g_current_usage -= (n <= g_current_usage) ? n : g_current_usage;
    g_free_count++;
    pthread_mutex_unlock(&g_stats_lock);
}

/* =========================================================================
 * Free-list helpers
 * ====================================================================== */

static void fl_push(SizeClass *sc, BlockHeader *h)
{
    FreeNode *n  = header_to_node(h);
    n->prev      = NULL;
    n->next      = sc->head;
    if (sc->head) sc->head->prev = n;
    sc->head     = n;
    sc->free_blocks++;
}

static BlockHeader *fl_pop(SizeClass *sc)
{
    if (!sc->head) return NULL;
    FreeNode *n  = sc->head;
    sc->head     = n->next;
    if (sc->head) sc->head->prev = NULL;
    sc->free_blocks--;
    return node_to_header(n);
}

static void fl_remove(SizeClass *sc, BlockHeader *h)
{
    FreeNode *n = header_to_node(h);
    if (n->prev) n->prev->next = n->next;
    else         sc->head      = n->next;
    if (n->next) n->next->prev = n->prev;
    sc->free_blocks--;
}

/* =========================================================================
 * Size-class lookup
 * ====================================================================== */

static int size_class_index(size_t size)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; ++i) {
        if (size <= g_size_classes[i]) return i;
    }
    return -1;   /* large */
}

/* =========================================================================
 * Coalesce: merge @h with adjacent free blocks
 * ====================================================================== */

static BlockHeader *_hs_coalesce(BlockHeader *h)
{
    int idx;

    /* Try to merge with next block */
    BlockHeader *next = next_header(h);
    /* Check next block is within arena */
    if ((void *)next < g_arena_top && !bh_in_use(next)) {
        idx = size_class_index(bh_size(next));
        if (idx >= 0) {
            pthread_mutex_lock(&g_classes[idx].lock);
            fl_remove(&g_classes[idx], next);
            pthread_mutex_unlock(&g_classes[idx].lock);
        }
        size_t new_size = bh_size(h) + bh_size(next);
        bh_set(h, new_size, 0, bh_prev_in_use(h));
        block_footer(h)->size = new_size;
        /* Update next-next's PREV_IN_USE */
        BlockHeader *nn = next_header(h);
        if ((void *)nn < g_arena_top)
            nn->size_flags &= ~BH_PREV_IN_USE;
    }

    /* Try to merge with previous block */
    if (!bh_prev_in_use(h)) {
        BlockHeader *prev = prev_header(h);
        idx = size_class_index(bh_size(prev));
        if (idx >= 0) {
            pthread_mutex_lock(&g_classes[idx].lock);
            fl_remove(&g_classes[idx], prev);
            pthread_mutex_unlock(&g_classes[idx].lock);
        }
        size_t new_size = bh_size(prev) + bh_size(h);
        bh_set(prev, new_size, 0, bh_prev_in_use(prev));
        block_footer(prev)->size = new_size;
        h = prev;
    }

    return h;
}

/* =========================================================================
 * Split: if @h is larger than needed, carve off the remainder
 * ====================================================================== */

static void _hs_split(BlockHeader *h, size_t needed)
{
    size_t total = bh_size(h);
    size_t remainder = total - needed;
    if (remainder < MIN_BLOCK + FOOTER_SIZE) return;  /* too small to split */

    /* Trim h */
    bh_set(h, needed, 1, bh_prev_in_use(h));

    /* Create remainder block */
    BlockHeader *rem = next_header(h);
    bh_set(rem, remainder, 0, 1 /* prev = h is in use */);
    block_footer(rem)->size = remainder;

    /* Update next block's prev_in_use */
    BlockHeader *nn = next_header(rem);
    if ((void *)nn < g_arena_top)
        nn->size_flags &= ~BH_PREV_IN_USE;

    /* Put remainder on free list */
    int idx = size_class_index(remainder - HEADER_SIZE - FOOTER_SIZE);
    if (idx >= 0) {
        pthread_mutex_lock(&g_classes[idx].lock);
        fl_push(&g_classes[idx], rem);
        pthread_mutex_unlock(&g_classes[idx].lock);
    }
    /* If remainder is too large for any class, just leave it untracked
     * (it will be coalesced later). */
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void hs_init(void)
{
    static int initialised = 0;
    if (initialised) return;
    initialised = 1;

    for (int i = 0; i < NUM_SIZE_CLASSES; ++i) {
        g_classes[i].head         = NULL;
        g_classes[i].block_size   = g_size_classes[i];
        g_classes[i].total_blocks = 0;
        g_classes[i].free_blocks  = 0;
        pthread_mutex_init(&g_classes[i].lock, NULL);
    }
}

void *hs_malloc(size_t size)
{
    if (size == 0) return NULL;

    size_t aligned = round_up(size, ALIGN);

    /* Large allocation: use mmap directly */
    if (aligned > LARGE_THRESHOLD) {
        size_t total = round_up(aligned + HEADER_SIZE, (size_t)getpagesize());
        pthread_mutex_lock(&g_large_lock);
        void *region = mmap(NULL, total,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        pthread_mutex_unlock(&g_large_lock);
        if (region == MAP_FAILED) return NULL;

        BlockHeader *h = (BlockHeader *)region;
        bh_set(h, total, 1, 1);
        stats_alloc(aligned);
        return header_to_payload(h);
    }

    int idx = size_class_index(aligned);
    if (idx < 0) return NULL;  /* shouldn't happen */

    size_t block_payload = g_size_classes[idx];
    size_t block_total   = HEADER_SIZE + block_payload + FOOTER_SIZE;

    pthread_mutex_lock(&g_classes[idx].lock);
    BlockHeader *h = fl_pop(&g_classes[idx]);
    pthread_mutex_unlock(&g_classes[idx].lock);

    if (h == NULL) {
        /* Allocate from arena */
        h = (BlockHeader *)arena_bump(block_total);
        if (h == NULL) return NULL;
        bh_set(h, block_total, 1, 1);
        block_footer(h)->size = block_total;

        pthread_mutex_lock(&g_classes[idx].lock);
        g_classes[idx].total_blocks++;
        pthread_mutex_unlock(&g_classes[idx].lock);
    } else {
        bh_set(h, bh_size(h), 1, bh_prev_in_use(h));
    }

    /* Update next block's prev_in_use */
    BlockHeader *next = next_header(h);
    if ((void *)next < g_arena_top)
        next->size_flags |= BH_PREV_IN_USE;

    _hs_split(h, HEADER_SIZE + aligned + FOOTER_SIZE);

    stats_alloc(aligned);
    return header_to_payload(h);
}

void *hs_calloc(size_t nmemb, size_t size)
{
    if (nmemb == 0 || size == 0) return NULL;
    /* Overflow check */
    if (size > SIZE_MAX / nmemb) return NULL;
    size_t total = nmemb * size;
    void *p = hs_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *hs_realloc(void *ptr, size_t size)
{
    if (ptr == NULL)  return hs_malloc(size);
    if (size == 0) { hs_free(ptr); return NULL; }

    BlockHeader *h      = payload_to_header(ptr);
    size_t       old_sz = bh_size(h) - HEADER_SIZE - FOOTER_SIZE;

    void *new_ptr = hs_malloc(size);
    if (new_ptr == NULL) return NULL;

    size_t copy = (old_sz < size) ? old_sz : size;
    memcpy(new_ptr, ptr, copy);
    hs_free(ptr);
    return new_ptr;
}

void hs_free(void *ptr)
{
    if (ptr == NULL) return;

    BlockHeader *h    = payload_to_header(ptr);
    size_t       size = bh_size(h);

    /* Large allocation: munmap */
    if (size > LARGE_THRESHOLD + HEADER_SIZE) {
        size_t payload = size - HEADER_SIZE;
        stats_free(payload);
        pthread_mutex_lock(&g_large_lock);
        munmap((void *)h, size);
        pthread_mutex_unlock(&g_large_lock);
        return;
    }

    size_t payload = size - HEADER_SIZE - FOOTER_SIZE;
    stats_free(payload);

    /* Mark free and write footer */
    bh_set(h, size, 0, bh_prev_in_use(h));
    block_footer(h)->size = size;

    /* Update next block's prev_in_use */
    BlockHeader *next = next_header(h);
    if ((void *)next < g_arena_top)
        next->size_flags &= ~BH_PREV_IN_USE;

    /* Coalesce with neighbours */
    h = _hs_coalesce(h);

    /* Add to appropriate free list */
    int idx = size_class_index(bh_size(h) - HEADER_SIZE - FOOTER_SIZE);
    if (idx >= 0) {
        pthread_mutex_lock(&g_classes[idx].lock);
        fl_push(&g_classes[idx], h);
        pthread_mutex_unlock(&g_classes[idx].lock);
    }
}

hs_stats_t hs_stats(void)
{
    hs_stats_t s;
    pthread_mutex_lock(&g_stats_lock);
    s.total_allocated    = g_total_allocated;
    s.total_freed        = g_total_freed;
    s.peak_usage         = g_peak_usage;
    s.current_usage      = g_current_usage;
    s.alloc_count        = g_alloc_count;
    s.free_count         = g_free_count;
    pthread_mutex_unlock(&g_stats_lock);

    pthread_mutex_lock(&g_arena_lock);
    size_t heap_sz = g_arena_total;
    pthread_mutex_unlock(&g_arena_lock);

    if (heap_sz > 0)
        s.fragmentation_ratio = 1.0 - ((double)s.current_usage / (double)heap_sz);
    else
        s.fragmentation_ratio = 0.0;

    return s;
}
