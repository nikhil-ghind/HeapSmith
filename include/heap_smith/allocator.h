#ifndef HEAP_SMITH_ALLOCATOR_H
#define HEAP_SMITH_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Statistics
 * ---------------------------------------------------------------------- */

typedef struct {
    size_t total_allocated;     /* cumulative bytes handed to callers */
    size_t total_freed;         /* cumulative bytes returned to allocator */
    size_t peak_usage;          /* maximum live bytes at any one time */
    size_t current_usage;       /* bytes currently live */
    double fragmentation_ratio; /* (heap_size - live_bytes) / heap_size */
    uint64_t alloc_count;       /* number of successful hs_malloc calls */
    uint64_t free_count;        /* number of hs_free calls */
} hs_stats_t;

/* -------------------------------------------------------------------------
 * Public allocator API
 * ---------------------------------------------------------------------- */

/**
 * hs_init - initialise the allocator.  Must be called once before any
 * other hs_* function.  Safe to call multiple times (idempotent).
 */
void hs_init(void);

/**
 * hs_malloc - allocate @size bytes.
 * Returns NULL on failure or if @size == 0.
 */
void *hs_malloc(size_t size);

/**
 * hs_calloc - allocate @nmemb * @size bytes, zero-initialised.
 * Returns NULL on overflow or failure.
 */
void *hs_calloc(size_t nmemb, size_t size);

/**
 * hs_realloc - resize the allocation pointed to by @ptr.
 * If @ptr is NULL, behaves like hs_malloc(@size).
 * If @size is 0, behaves like hs_free(@ptr) and returns NULL.
 */
void *hs_realloc(void *ptr, size_t size);

/**
 * hs_free - release a block previously returned by hs_malloc/calloc/realloc.
 * Passing NULL is safe and has no effect.
 */
void hs_free(void *ptr);

/**
 * hs_stats - return a snapshot of allocator statistics.
 */
hs_stats_t hs_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* HEAP_SMITH_ALLOCATOR_H */
