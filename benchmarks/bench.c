#define _GNU_SOURCE
#include "heap_smith/allocator.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Utilities
 * ====================================================================== */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* =========================================================================
 * Throughput benchmark
 *
 * N threads, each performing 100K malloc/free cycles.
 * Measures aggregate operations per second.
 * ====================================================================== */

#define THROUGHPUT_ITERS  100000

typedef struct {
    int    thread_id;
    int    n_threads;
    int    use_hs;      /* 1 = hs_malloc, 0 = system malloc */
    double ops_per_sec;
} ThroughputArg;

static void *throughput_worker(void *varg)
{
    ThroughputArg *a = (ThroughputArg *)varg;
    uint64_t start = now_ns();

    for (int i = 0; i < THROUGHPUT_ITERS; ++i) {
        size_t sz = ((size_t)(i % 16) + 1) * 8;
        void *p = a->use_hs ? hs_malloc(sz) : malloc(sz);
        if (p) {
            /* Touch memory to prevent optimisation */
            *(volatile char *)p = (char)i;
            if (a->use_hs) hs_free(p);
            else           free(p);
        }
    }

    uint64_t elapsed = now_ns() - start;
    a->ops_per_sec = (double)THROUGHPUT_ITERS / ((double)elapsed / 1e9);
    return NULL;
}

static double bench_throughput(int n_threads, int use_hs)
{
    pthread_t     threads[32];
    ThroughputArg args[32];
    if (n_threads > 32) n_threads = 32;

    for (int i = 0; i < n_threads; ++i) {
        args[i].thread_id  = i;
        args[i].n_threads  = n_threads;
        args[i].use_hs     = use_hs;
        args[i].ops_per_sec = 0.0;
        pthread_create(&threads[i], NULL, throughput_worker, &args[i]);
    }
    double total = 0.0;
    for (int i = 0; i < n_threads; ++i) {
        pthread_join(threads[i], NULL);
        total += args[i].ops_per_sec;
    }
    return total;
}

/* =========================================================================
 * Fragmentation benchmark
 *
 * Allocate 1024 random-ish sized blocks, then free every other one.
 * Measure fragmentation ratio via hs_stats().
 * ====================================================================== */

static void bench_fragmentation(void)
{
    printf("\n-- Fragmentation Benchmark --\n");
#define FRAG_BLOCKS 1024
    void *ptrs[FRAG_BLOCKS];

    for (int i = 0; i < FRAG_BLOCKS; ++i) {
        size_t sz = ((size_t)(i % 16) + 1) * 16;
        ptrs[i] = hs_malloc(sz);
    }

    /* Free every other block */
    for (int i = 0; i < FRAG_BLOCKS; i += 2)
        if (ptrs[i]) hs_free(ptrs[i]);

    hs_stats_t s = hs_stats();
    printf("  Blocks allocated : %d\n", FRAG_BLOCKS);
    printf("  Blocks freed     : %d (every other)\n", FRAG_BLOCKS / 2);
    printf("  Current usage    : %zu bytes\n", s.current_usage);
    printf("  Fragmentation    : %.2f%%\n", s.fragmentation_ratio * 100.0);

    /* Clean up */
    for (int i = 1; i < FRAG_BLOCKS; i += 2)
        if (ptrs[i]) hs_free(ptrs[i]);
}

/* =========================================================================
 * Thread scaling benchmark
 * ====================================================================== */

static void bench_thread_scale(void)
{
    printf("\n-- Thread Scaling Benchmark (ops/sec, 100K iters/thread) --\n");
    printf("  %-10s  %-20s  %-20s  %-10s\n",
           "Threads", "hs_malloc", "system malloc", "Speedup");
    printf("  %-10s  %-20s  %-20s  %-10s\n",
           "-------", "---------", "-------------", "-------");

    int thread_counts[] = { 1, 2, 4, 8, 16 };
    int n = (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));

    for (int i = 0; i < n; ++i) {
        int t = thread_counts[i];
        double hs  = bench_throughput(t, 1);
        double sys = bench_throughput(t, 0);
        printf("  %-10d  %-20.0f  %-20.0f  %-10.2fx\n",
               t, hs, sys, (sys > 0) ? hs / sys : 0.0);
    }
}

/* =========================================================================
 * Side-by-side comparison (single thread)
 * ====================================================================== */

static void bench_vs_system(void)
{
    printf("\n-- hs_malloc vs system malloc (single thread) --\n");

    static const struct { const char *name; size_t size; } cases[] = {
        { "small  (8B)",   8   },
        { "small  (32B)",  32  },
        { "medium (128B)", 128 },
        { "medium (512B)", 512 },
        { "large  (4KB)",  4096},
        { "large  (16KB)", 16384},
    };
    int ncases = (int)(sizeof(cases) / sizeof(cases[0]));

    printf("  %-20s  %-15s  %-15s  %-10s\n",
           "Operation", "hs_malloc", "sys malloc", "Speedup");
    printf("  %-20s  %-15s  %-15s  %-10s\n",
           "---------", "---------", "----------", "-------");

#define BENCH_ITERS 200000
    for (int c = 0; c < ncases; ++c) {
        size_t sz = cases[c].size;

        /* hs_malloc */
        uint64_t t0 = now_ns();
        for (int i = 0; i < BENCH_ITERS; ++i) {
            void *p = hs_malloc(sz);
            if (p) { *(volatile char *)p = 1; hs_free(p); }
        }
        double hs_ops = (double)BENCH_ITERS / ((double)(now_ns() - t0) / 1e9);

        /* system malloc */
        t0 = now_ns();
        for (int i = 0; i < BENCH_ITERS; ++i) {
            void *p = malloc(sz);
            if (p) { *(volatile char *)p = 1; free(p); }
        }
        double sys_ops = (double)BENCH_ITERS / ((double)(now_ns() - t0) / 1e9);

        printf("  %-20s  %-15.0f  %-15.0f  %-10.2fx\n",
               cases[c].name, hs_ops, sys_ops,
               (sys_ops > 0) ? hs_ops / sys_ops : 0.0);
    }
}

/* =========================================================================
 * main
 * ====================================================================== */

int main(void)
{
    hs_init();

    printf("=== HeapSmith Benchmark Suite ===\n");

    bench_vs_system();
    bench_thread_scale();
    bench_fragmentation();

    printf("\nDone.\n");
    return 0;
}
