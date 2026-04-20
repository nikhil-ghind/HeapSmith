cmake_minimum_required(VERSION 3.14)
project(HeapSmith C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Optimised release-style compile flags
add_compile_options(
    -O2
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    -Werror
)

# Thread support
find_package(Threads REQUIRED)

# -------------------------------------------------------------------------
# Allocator library
# -------------------------------------------------------------------------
add_library(heap_smith STATIC
    src/allocator.c
    src/thread_cache.c
    src/lock.c
)

target_include_directories(heap_smith
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(heap_smith
    PUBLIC Threads::Threads
)

# -------------------------------------------------------------------------
# Tests
# -------------------------------------------------------------------------
enable_testing()

add_executable(test_allocator tests/test_allocator.c)
target_link_libraries(test_allocator PRIVATE heap_smith)

add_test(NAME test_allocator COMMAND test_allocator)
set_tests_properties(test_allocator PROPERTIES TIMEOUT 30)

# -------------------------------------------------------------------------
# Benchmarks
# -------------------------------------------------------------------------
add_executable(bench benchmarks/bench.c)
target_link_libraries(bench PRIVATE heap_smith)

# -------------------------------------------------------------------------
# LD_PRELOAD shared library (optional install target)
#
# Build with:  cmake --build build --target heap_smith_preload
# Use with:    LD_PRELOAD=./build/libheap_smith_preload.so your_program
# -------------------------------------------------------------------------
add_library(heap_smith_preload SHARED src/allocator.c src/thread_cache.c src/lock.c)
target_include_directories(heap_smith_preload
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(heap_smith_preload PUBLIC Threads::Threads)

# Wrap glibc symbols so the preload library intercepts malloc/free
target_compile_definitions(heap_smith_preload PRIVATE HS_PRELOAD=1)

# Provide malloc/free/calloc/realloc symbols for LD_PRELOAD usage
# by creating a thin wrapper source inline
file(WRITE ${CMAKE_BINARY_DIR}/preload_wrap.c
"#include \"heap_smith/allocator.h\"
#include <stddef.h>
void* malloc(size_t n)              { return hs_malloc(n); }
void  free(void* p)                 { hs_free(p); }
void* calloc(size_t n, size_t s)    { return hs_calloc(n, s); }
void* realloc(void* p, size_t n)    { return hs_realloc(p, n); }
__attribute__((constructor)) static void _hs_preload_init(void) { hs_init(); }
")

target_sources(heap_smith_preload PRIVATE ${CMAKE_BINARY_DIR}/preload_wrap.c)
