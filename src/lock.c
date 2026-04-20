#define _GNU_SOURCE
#include <pthread.h>
#include <stddef.h>

/* =========================================================================
 * Fine-grained locking layer
 *
 * Each size class has its own pthread_mutex_t so that threads allocating
 * different size classes do not contend with each other.  Large allocations
 * (> 4096 bytes) share a single lock because they use mmap and are
 * relatively infrequent.
 *
 * The lock instances themselves live in allocator.c (inside the SizeClass
 * structs) and in the g_large_lock global.  This file documents the design
 * and provides helper wrappers if needed for testing.
 * ====================================================================== */

/*
 * Lock design summary:
 *
 *  - g_classes[i].lock  — guards the free list and counters for size class i
 *  - g_large_lock       — guards the mmap/munmap path for large blocks
 *  - g_arena_lock       — guards the bump-pointer arena (sbrk)
 *  - g_stats_lock       — guards the global statistics counters
 *
 * Thread cache (thread_cache.c) is __thread-local and requires no locking.
 *
 * Lock ordering (to prevent deadlock): never acquire a coarser lock while
 * holding a finer one.  The defined order is:
 *   g_arena_lock > g_large_lock > g_classes[i].lock (lower index first)
 *
 * Coalesce (_hs_coalesce) may need to remove a block from an adjacent
 * size class's free list.  It always acquires at most one class lock at a
 * time and releases it before acquiring another, avoiding potential
 * deadlocks.
 */

/* Exposed for unit-test introspection */
void lock_noop(void) { /* intentionally empty */ }
