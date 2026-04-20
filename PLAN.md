# Heap Smith

## Project Overview

A thread-safe heap allocator implemented in C++17 using segregated free lists with mutex-protected critical sections and an aggressive coalescing strategy. The allocator replaces system `malloc`/`free` for targeted workloads, achieving 30% lower fragmentation under embedded-style allocation patterns (many small, short-lived objects). A benchmarking harness compares throughput and fragmentation against glibc's `ptmalloc2` and Google's `tcmalloc`.

**Key Goals:**
- Correct multi-threaded allocation/deallocation with no data races (verified by TSan).
- Segregated free lists covering size classes from 8 bytes to 64 KB (power-of-2 buckets).
- Immediate and deferred coalescing strategies, selectable at compile time.
- Benchmark report showing throughput (ops/sec) and peak fragmentation ratio vs. system malloc.

---

## Tech Stack

| Layer | Tool / Library | Version |
|---|---|---|
| Core language | C++ | C++17 |
| Build system | CMake | 3.22+ |
| Unit testing | Google Test | 1.14.0 |
| Thread sanitizer | TSan (GCC/Clang built-in) | — |
| Address sanitizer | ASan | — |
| Benchmarking | Google Benchmark | 1.8.3 |
| System allocator reference | glibc ptmalloc2 | system |
| OS | Linux (Ubuntu 22.04+) | — |
| Threading | POSIX Threads (std::thread wrapper) | — |

---

## Architecture Overview

```
Application (malloc / free calls)
         |
         v
+----------------------------+
|   AllocatorFacade          |  <-- drop-in malloc/free/realloc/calloc wrappers
+----------------------------+
         |
         v
+----------------------------+
|   SegregatedAllocator      |  <-- routes to the correct SizeClass bucket
|   (thread-safe dispatcher) |
+----------------------------+
   |       |       |       |
   v       v       v       v
[Bucket0][Bucket1]...[BucketN]   <-- one per size class (8B, 16B, ..., 64KB+)
   Each bucket:
     pthread_mutex_t lock
     FreeList (doubly-linked list of FreeBlock nodes)
     HeapArena (mmap-backed slab for this size class)
         |
         v
+----------------------------+
|   Coalescer                |  <-- merges adjacent free blocks, updates free list
+----------------------------+
         |
         v
+----------------------------+
|   ArenaManager             |  <-- mmap / munmap slabs; tracks total mapped memory
+----------------------------+
```

---

## Phase 1 — Project Scaffold

**Goal:** CMake project with Google Test and Google Benchmark wired up, compiling cleanly.

### Tasks

1. Create directory layout:
   ```
   multiThreadedMemoryAllocator/
   ├── CMakeLists.txt
   ├── include/
   │   └── allocator/
   │       ├── allocator_facade.h
   │       ├── segregated_allocator.h
   │       ├── free_list.h
   │       ├── block_header.h
   │       ├── arena_manager.h
   │       ├── coalescer.h
   │       └── size_class.h
   ├── src/
   │   ├── allocator_facade.cpp
   │   ├── segregated_allocator.cpp
   │   ├── free_list.cpp
   │   ├── arena_manager.cpp
   │   └── coalescer.cpp
   ├── tests/
   │   ├── CMakeLists.txt
   │   ├── test_free_list.cpp
   │   ├── test_segregated_allocator.cpp
   │   ├── test_coalescer.cpp
   │   └── test_thread_safety.cpp
   ├── bench/
   │   ├── CMakeLists.txt
   │   └── bench_allocator.cpp
   └── scripts/
       └── run_benchmarks.sh
   ```

2. **`CMakeLists.txt`** (root):
   - C++17, `-Wall -Wextra -O2 -pthread`.
   - `FetchContent` for GoogleTest 1.14.0 and GoogleBenchmark 1.8.3.
   - Library target `allocator_lib` from `src/*.cpp`.
   - Add `tests/` and `bench/` subdirectories.
   - Add a CMake option `ENABLE_TSAN` — when ON, append `-fsanitize=thread` to compile/link flags.
   - Add a CMake option `ENABLE_ASAN` — when ON, append `-fsanitize=address,undefined`.

3. **`tests/CMakeLists.txt`**: link `allocator_lib GTest::gtest_main -lpthread`.

4. **`bench/CMakeLists.txt`**: link `allocator_lib benchmark::benchmark -lpthread`.

5. Initial build check:
   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j$(nproc)
   ```

---

## Phase 2 — Block Header and Size Class Definitions

**Goal:** Define the in-band block metadata and size-class mapping.

### Tasks

1. **`include/allocator/block_header.h`** — define `struct BlockHeader`:
   - Fields: `size_t size` (includes header), `bool is_free`, `BlockHeader* prev_physical`, `BlockHeader* next_physical` (for coalescing), `BlockHeader* prev_free`, `BlockHeader* next_free` (for free list links).
   - Static methods: `BlockHeader* fromPayload(void* ptr)` — subtract `sizeof(BlockHeader)`.
   - `void* payload()` — return `this + 1`.
   - `BlockHeader* nextPhysical()` — return `reinterpret_cast<BlockHeader*>(payload()) + size - sizeof(BlockHeader)` — careful pointer arithmetic.

2. **`include/allocator/size_class.h`** — define:
   - `constexpr size_t SIZE_CLASSES[] = {8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536}`.
   - `constexpr size_t NUM_CLASSES = std::size(SIZE_CLASSES)`.
   - `size_t sizeClassIndex(size_t request)` — returns index of smallest class >= request; uses a compile-time lookup via constexpr loop.
   - `size_t alignUp(size_t n, size_t align)` — round up to alignment boundary.

---

## Phase 3 — Free List and Arena Manager

**Goal:** Implement the per-bucket intrusive doubly-linked free list and the mmap-based arena.

### Tasks

1. **`include/allocator/free_list.h`** — declare `class FreeList`:
   - Fields: `BlockHeader* head_`, `size_t count_`.
   - Methods: `void insert(BlockHeader* blk)`, `BlockHeader* remove(BlockHeader* blk)`, `BlockHeader* findFirst(size_t min_size)`, `size_t count() const`.

2. **`src/free_list.cpp`** — implement:
   - `insert()`: push to front (O(1)); update `prev_free` / `next_free` pointers.
   - `remove()`: unlink from doubly-linked list; decrement count.
   - `findFirst()`: linear scan returning first block with `block->size >= min_size + sizeof(BlockHeader)`.

3. **`include/allocator/arena_manager.h`** — declare `class ArenaManager` (singleton):
   - Fields: `std::vector<void*> slabs_`, `size_t total_mapped_bytes_`, `pthread_mutex_t lock_`.
   - Methods: `void* allocateSlab(size_t size)` — calls `mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)`.
   - `void freeSlab(void* ptr, size_t size)` — calls `munmap`.
   - `size_t totalMapped() const`.

4. **`src/arena_manager.cpp`** — implement:
   - Default slab size: `constexpr size_t SLAB_SIZE = 2 * 1024 * 1024` (2 MB).
   - `allocateSlab()`: round up requested size to SLAB_SIZE multiple, mmap, push to `slabs_`.
   - Initialize the returned slab as a single large `BlockHeader` spanning the entire slab.

---

## Phase 4 — Segregated Allocator Core

**Goal:** Implement the central dispatcher that routes allocations to the correct bucket and manages splits and coalescing.

### Tasks

1. **`include/allocator/segregated_allocator.h`** — declare `class SegregatedAllocator` (singleton):
   - Fields: `FreeList free_lists_[NUM_CLASSES]`, `pthread_mutex_t locks_[NUM_CLASSES]`, `ArenaManager& arena_`.
   - Public: `void* allocate(size_t size)`, `void deallocate(void* ptr)`, `void* reallocate(void* ptr, size_t new_size)`.
   - Private: `BlockHeader* splitBlock(BlockHeader* blk, size_t needed_size)`, `void coalesce(BlockHeader* blk)`.

2. **`src/segregated_allocator.cpp`** — implement:
   - `allocate(size)`:
     1. Compute `idx = sizeClassIndex(alignUp(size, 8))`.
     2. Lock `locks_[idx]`.
     3. Call `free_lists_[idx].findFirst(size)`.
     4. If found: call `splitBlock()` if remainder >= min_useful_size (32 bytes), remove from free list, mark as allocated, unlock, return payload.
     5. If not found: unlock, request a new slab from ArenaManager, initialize as one giant free block, lock, insert into appropriate free list, retry.
   - `deallocate(ptr)`:
     1. Recover `BlockHeader* blk = BlockHeader::fromPayload(ptr)`.
     2. Mark as free.
     3. Call `coalesce(blk)` to merge with physical neighbors.
     4. Determine size class, lock, insert into free list, unlock.
   - `splitBlock(blk, needed)`:
     - If `blk->size - needed > sizeof(BlockHeader) + 32`: carve off a new `BlockHeader` for the remainder, insert remainder into its size class's free list; return original block trimmed to `needed`.

3. **`src/coalescer.cpp`** — implement `void Coalescer::coalesce(BlockHeader* blk, SegregatedAllocator& alloc)`:
   - Check `blk->prev_physical`: if free, remove from free list, merge (update size, update physical chain).
   - Check `blk->next_physical`: if free, remove from free list, merge.

---

## Phase 5 — Allocator Facade (Drop-in Wrappers)

**Goal:** Provide `malloc` / `free` / `calloc` / `realloc` compatible wrappers.

### Tasks

1. **`include/allocator/allocator_facade.h`** — declare:
   ```cpp
   void* sf_malloc(size_t size);
   void  sf_free(void* ptr);
   void* sf_calloc(size_t nmemb, size_t size);
   void* sf_realloc(void* ptr, size_t new_size);
   ```

2. **`src/allocator_facade.cpp`** — implement:
   - `sf_malloc()`: forward to `SegregatedAllocator::getInstance().allocate(size)`.
   - `sf_free()`: guard against nullptr, forward to `deallocate`.
   - `sf_calloc()`: multiply, check overflow via `__builtin_mul_overflow`, allocate, `memset` to zero.
   - `sf_realloc()`: if ptr is null, behave as `sf_malloc`; otherwise `allocate(new_size)`, `memcpy(min(old_size, new_size))`, `sf_free(ptr)`.

---

## Phase 6 — Unit Tests

**Goal:** Thorough correctness coverage including thread safety.

### Tasks

1. **`tests/test_free_list.cpp`**:
   - `InsertAndFindFirst` — insert 5 blocks of varying sizes; verify `findFirst` returns correct minimum.
   - `RemoveFromMiddle` — insert 3, remove middle, verify list integrity.
   - `EmptyListReturnsNull` — `findFirst` on empty list returns nullptr.

2. **`tests/test_segregated_allocator.cpp`**:
   - `AllocateSmall` — allocate 8 bytes; payload pointer non-null; write and read back.
   - `AllocateLarge` — allocate 32768 bytes; write pattern; verify.
   - `FreeAndReallocate` — allocate, free, reallocate same size; verify no crash.
   - `CoalescingReducesFragmentation` — allocate 1000 × 64-byte blocks, free all; then allocate one 60000-byte block; verify it succeeds (coalescing worked).

3. **`tests/test_thread_safety.cpp`**:
   - `ConcurrentAllocFree` — 16 threads each doing 10000 allocate/free cycles on random sizes 8–4096; run under TSan; expect zero races.
   - `ProducerConsumerSharedBuffer` — producer allocates, consumer frees; verify no use-after-free.

4. Run with TSan:
   ```bash
   cmake -B build_tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
   cmake --build build_tsan -j$(nproc)
   cd build_tsan && ctest --output-on-failure
   ```

---

## Phase 7 — Benchmarks

**Goal:** Quantify throughput and fragmentation vs. system malloc.

### Tasks

1. **`bench/bench_allocator.cpp`** — implement Google Benchmark cases:
   - `BM_SfMallocSmallSequential` — single-threaded 1M × 32-byte allocate/free.
   - `BM_SystemMallocSmallSequential` — same with `::malloc`/`::free`.
   - `BM_SfMallocMixed` — single-threaded random sizes 8–4096.
   - `BM_SfMallocMultiThread` — 8 threads × 100k mixed-size alloc/free; use `BENCHMARK_THREADS`.
   - `BM_SystemMallocMultiThread` — same with system malloc.
   - `BM_FragmentationRatio` — allocate/free in a pattern that maximizes fragmentation; report `arena.totalMapped() / sum_of_live_bytes`.

2. **`scripts/run_benchmarks.sh`**:
   ```bash
   #!/usr/bin/env bash
   set -euo pipefail
   cmake -B build_bench -DCMAKE_BUILD_TYPE=Release
   cmake --build build_bench --target bench_allocator -j$(nproc)
   ./build_bench/bench/bench_allocator --benchmark_format=json --benchmark_out=bench_results.json
   echo "Results written to bench_results.json"
   ```

3. Target metrics (to document in test comments):
   - sf_malloc throughput >= 80% of system malloc for sequential workloads.
   - Fragmentation ratio <= 70% of system malloc under embedded workload (many 32–256 byte short-lived allocs).
