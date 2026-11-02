# nanoalloc

**nanoalloc is a C++20 memory allocator project focused on combining memory
efficiency with runtime efficiency. Its goal is to minimize wasted memory
without sacrificing the speed required by performance-sensitive
applications.**

Version 0.1 is the measured starting point: a conventional, carefully tested
binary buddy allocator behind a stable, algorithm-neutral API, with a
benchmark suite that reports both speed and memory. Future versions will
replace the allocation engine; the API, the contract tests and the benchmarks
are designed to stay the same so that every change can be measured against
this baseline.

- [The problem](#the-problem)
- [What version 0.1 provides](#what-version-01-provides)
- [Building, testing and benchmarking](#building-testing-and-benchmarking)
- [Usage](#usage)
- [API reference](#api-reference)
- [Memory accounting](#memory-accounting)
- [Benchmarks](#benchmarks)
- [Testing](#testing)
- [Design of the v0.1 engine](#design-of-the-v01-engine)
- [Limitations](#limitations)
- [Roadmap](#roadmap)

## The problem

Allocators usually trade one resource for the other. Fast general-purpose
allocators round requests up to size classes, keep per-thread caches of free
blocks and store headers next to each allocation; all of that costs memory.
Compact allocators fit requests tightly but tend to spend time searching,
splitting and coalescing.

Both costs matter. Memory that is allocated but never used still occupies
cache lines, TLB entries, pages and, at scale, machines. Allocation time sits
on the hot path of containers, parsers, graph and simulation code, and
servers. nanoalloc aims for an allocator that does not force that choice.

The long-term objective is **16-byte granular allocation**: every block
16-byte aligned and **at most 15 bytes of unused payload capacity per
allocation**, at a speed competitive with mainstream allocators. That target
concerns payload rounding only. Metadata, fragmentation, and reserved but
unused arena space are tracked and reported separately, because a low
rounding waste is worthless if it is paid for elsewhere.

## What version 0.1 provides

- `nanoalloc::Pool`: an arena-backed allocator. One contiguous arena is
  reserved at construction (RAII); allocation and deallocation never call the
  system allocator, and an exhausted arena is reported, never silently
  redirected to `malloc`/`new`.
- A traditional **binary buddy engine**: power-of-two blocks from 16 bytes up,
  splitting on allocation and coalescing on release, with no per-allocation
  header.
- 16-byte minimum alignment; any power-of-two alignment up to 4096 bytes.
- Size-free deallocation: `deallocate(ptr)` and `usable_size(ptr)` need only
  the pointer.
- `nanoalloc::MemoryResource`, a `std::pmr::memory_resource` adapter.
- `nanoalloc::Allocator<T>`, a C++20 standard allocator for ordinary STL
  containers.
- `PoolStats`: requested, usable and consumed bytes (live and peak), free
  space, the largest available block, metadata size, and allocation, failure,
  split and merge counters.
- Cheap misuse detection (foreign pointers, pointers into the middle of a
  block, most double frees) and AddressSanitizer integration that poisons free
  arena memory.
- Dependency-free tests (contract, stress, engine, misuse) and a
  dependency-free benchmark executable, both runnable on Linux and Windows.

The buddy engine is deliberately a baseline. Power-of-two rounding wastes up
to half of each block (about a third on the benchmark workloads, see
[Benchmarks](#benchmarks)), which is far from the 15-byte target. Its value
is that it is simple, well understood, easy to verify, and therefore a
trustworthy reference point for the optimized engines that follow.

## Building, testing and benchmarking

Requirements: CMake 3.20 or newer and a C++20 compiler (tested with GCC 13
and Clang 18 on Linux; the CI configuration also builds with MSVC 2022 on
Windows). There are no third-party dependencies.

With presets (Ninja; presets require CMake 3.21):

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
./build/release/benchmarks/nanoalloc_bench
```

Without presets (any generator):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/benchmarks/nanoalloc_bench
```

With AddressSanitizer and UndefinedBehaviorSanitizer (GCC or Clang):

```bash
cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize
```

On Windows, `cmake --preset msvc` followed by
`cmake --build --preset msvc-release` and `ctest --preset msvc-release` uses
Visual Studio 2022; the `msvc-asan` presets enable MSVC's AddressSanitizer.

Useful CMake options:

| Option | Default | Meaning |
| --- | --- | --- |
| `NANOALLOC_BUILD_TESTS` | ON at top level | Build the test suite |
| `NANOALLOC_BUILD_BENCHMARKS` | ON at top level | Build `nanoalloc_bench` |
| `NANOALLOC_BUILD_EXAMPLES` | ON at top level | Build (and test-run) the examples |
| `NANOALLOC_SANITIZE` | empty | e.g. `address;undefined` |
| `NANOALLOC_WARNINGS_AS_ERRORS` | OFF | `-Werror` / `/WX` |
| `BUILD_SHARED_LIBS` | OFF | Build a shared library |

The single-config default build type is `Release`.

### Using nanoalloc from another CMake project

```cmake
# After `cmake --install build --prefix <prefix>`:
find_package(nanoalloc 0.1 REQUIRED)
target_link_libraries(my_app PRIVATE nanoalloc::nanoalloc)

# Or vendored:
add_subdirectory(third_party/nanoalloc)
target_link_libraries(my_app PRIVATE nanoalloc::nanoalloc)
```

## Usage

### Pool

```cpp
#include <nanoalloc/pool.hpp>

#include <cstdio>

int main() {
    nanoalloc::Pool pool({1024 * 1024});  // 1 MiB arena

    void* data = pool.allocate(100);      // 16-byte aligned
    void* line = pool.allocate(64, 64);   // 64-byte aligned

    std::printf("usable: %zu bytes\n", pool.usable_size(data));

    pool.deallocate(line);  // no size needed
    pool.deallocate(data);

    const nanoalloc::PoolStats stats = pool.stats();
    std::printf("live: %zu, free: %zu of %zu bytes\n", stats.live_allocations,
                stats.free_bytes, stats.capacity_bytes);
}
```

`allocate()` throws `std::bad_alloc` when the request cannot be satisfied;
`try_allocate()` returns `nullptr` instead.

### std::pmr containers

```cpp
#include <nanoalloc/memory_resource.hpp>
#include <nanoalloc/pool.hpp>

#include <memory_resource>
#include <string>
#include <vector>

int main() {
    nanoalloc::Pool pool({64 * 1024 * 1024});
    nanoalloc::MemoryResource resource(pool);

    std::pmr::vector<int> values(&resource);
    values.push_back(42);

    // Nested pmr containers pass the resource on to their elements.
    std::pmr::vector<std::pmr::string> names(&resource);
    names.emplace_back("a string long enough to need dynamic storage");
}
```

### Standard allocator

```cpp
#include <nanoalloc/allocator.hpp>
#include <nanoalloc/pool.hpp>

#include <functional>
#include <list>
#include <map>
#include <vector>

int main() {
    nanoalloc::Pool pool({16 * 1024 * 1024});

    std::vector<int, nanoalloc::Allocator<int>> values{nanoalloc::Allocator<int>(pool)};
    values.assign({3, 1, 4, 1, 5});

    // Node-based containers rebind the allocator automatically.
    std::list<double, nanoalloc::Allocator<double>> samples{nanoalloc::Allocator<double>(pool)};
    samples.push_back(0.5);

    using Entry = std::pair<const int, int>;
    std::map<int, int, std::less<>, nanoalloc::Allocator<Entry>> index{
        nanoalloc::Allocator<Entry>(pool)};
    index[1] = 2;
}
```

Complete, runnable versions are in [`examples/`](examples); they are built and
executed by the test suite.

### Lifetime and threading rules

- The `Pool` must outlive every allocation, every `MemoryResource` and
  `Allocator` that refers to it, and every container using them. The adapters
  never own the pool. Destroying a pool releases its whole arena at once and
  does not run destructors of objects still living in it.
- A `Pool` is not copyable or movable (adapters hold its address). Use
  `std::unique_ptr<nanoalloc::Pool>` for a movable handle.
- **A `Pool` is not thread-safe.** Every call on the same pool, including
  const ones such as `stats()` and `usable_size()`, must be externally
  synchronized; concurrent calls are data races. Separate pools may be
  used from separate threads. The engine deliberately has no locks; concurrent
  support will be added when there is a measured need for it.

## API reference

All public names are in namespace `nanoalloc`; headers are under
`include/nanoalloc/`. `<nanoalloc/nanoalloc.hpp>` includes everything. The
public headers contain no engine-specific types.

### PoolConfig

| Member | Default | Meaning |
| --- | --- | --- |
| `capacity_bytes` | 0 (invalid) | Arena size; rounded up to a multiple of 16 |
| `track_requested_bytes` | `false` | Keep exact requested-byte statistics (see below) |

### Pool

| Member | Behavior |
| --- | --- |
| `explicit Pool(const PoolConfig&)` | Reserves the arena. `std::invalid_argument` if capacity is 0 or above `max_capacity`; `std::bad_alloc` if memory cannot be obtained |
| `void* allocate(bytes, alignment = 16)` | At least `bytes` bytes, aligned to `max(alignment, 16)`. Throws `std::invalid_argument` for a zero or non-power-of-two alignment, `std::bad_alloc` if it cannot be satisfied (including alignment above 4096) |
| `void* try_allocate(bytes, alignment = 16) noexcept` | As `allocate`, returning `nullptr` on any failure |
| `void deallocate(void* ptr) noexcept` | Releases `ptr`; `nullptr` is a no-op |
| `size_t usable_size(const void* ptr) const noexcept` | Bytes the caller may use at `ptr` (>= requested); 0 for `nullptr` |
| `bool owns(const void* ptr) const noexcept` | Whether `ptr` lies inside the arena |
| `size_t capacity() const noexcept` | Arena size |
| `PoolStats stats() const noexcept` | Statistics snapshot, O(1) |
| `bool verify_integrity() const noexcept` | Full internal consistency check, O(capacity); for tests and debugging |
| `min_alignment`, `max_alignment`, `granularity`, `max_capacity` | 16, 4096, 16, 2^62 (2^30 on 32-bit) |

Contract details:

- **Zero-byte requests** return a unique, non-null, 16-byte aligned pointer
  that must be released like any other allocation.
- **Failure** leaves the pool unchanged apart from
  `PoolStats::failed_allocation_count` (strong exception guarantee).
- **No fallback**: when the arena cannot satisfy a request, the request fails.
- **Recovery**: once every allocation has been released, `free_bytes` equals
  `capacity_bytes` and `largest_free_block` returns to its initial value.
- **Misuse** (a pointer not returned by this pool, already released, or into
  the middle of a block) is undefined behavior. The pool detects the common
  cases at negligible cost and terminates with a
  `nanoalloc: fatal error: ...` diagnostic instead of corrupting itself.

### MemoryResource and Allocator&lt;T&gt;

`MemoryResource(Pool&)` implements `std::pmr::memory_resource`. Two resources
compare equal when they refer to the same pool. Allocation failures throw
`std::bad_alloc`, alignments follow the pool's rules.

`Allocator<T>(Pool&)` is a one-pointer handle satisfying the C++20 Allocator
requirements:

- rebinding preserves the pool; allocators are equal iff their pools are the
  same (`is_always_equal` is false);
- it propagates on container copy assignment, move assignment and swap, so a
  container always draws from the pool its allocator names, and move
  assignment never copies elements;
- storage is aligned to `max(alignof(T), 16)`; `alignof(T) > 4096` is rejected
  at compile time;
- it is not default-constructible, so a pool must always be named;
- `allocate(n)` throws `std::bad_array_new_length` if `n * sizeof(T)`
  overflows and `std::bad_alloc` if the pool is exhausted.

## Memory accounting

`PoolStats` distinguishes four quantities:

| Term | Meaning |
| --- | --- |
| requested | the `bytes` argument passed to `allocate` |
| usable | what `usable_size()` reports; the caller may use all of it (>= requested) |
| consumed | arena bytes made unavailable by an allocation, including any in-arena per-allocation overhead (>= usable; equal for the buddy engine, which has no headers) |
| reserved | memory obtained from the system: the arena (`capacity_bytes`) plus bookkeeping (`metadata_bytes`, `tracking_bytes`) |

*Payload slack* is usable − requested. *Fragmentation* is free arena memory
that cannot serve a request because it is not contiguous; compare
`free_bytes` with `largest_free_block`.

| Field | Meaning |
| --- | --- |
| `capacity_bytes` | Arena size |
| `metadata_bytes` | Engine bookkeeping held outside the arena (allocated at construction) |
| `tracking_bytes` | Requested-size table; 0 unless tracking is enabled |
| `live_allocations` | Allocations not yet released |
| `live_requested_bytes`, `peak_requested_bytes` | Sum of live requested sizes, and its maximum; 0 unless tracking is enabled |
| `live_usable_bytes`, `peak_usable_bytes` | Sum of live usable sizes, and its maximum |
| `live_consumed_bytes`, `peak_consumed_bytes` | Arena bytes consumed by live allocations, and its maximum |
| `free_bytes` | `capacity_bytes - live_consumed_bytes` (not necessarily contiguous) |
| `largest_free_block` | Any request with `bytes <= largest_free_block` and alignment <= 16 succeeds now |
| `allocation_count`, `deallocation_count` | Successful allocations; deallocations of non-null pointers |
| `failed_allocation_count` | `allocate`/`try_allocate` calls that returned no memory |
| `split_count`, `merge_count` | Block divisions and coalescings performed by the engine |
| `requested_bytes_tracked` | Whether the requested-byte fields are meaningful |

Peaks are tracked independently and may have been reached at different
moments. Statistics cost a few register-level counter updates per operation.

**Why requested-byte tracking is optional.** `deallocate()` receives no size,
so exact requested-byte statistics need a side table (one byte per 16-byte
granule). Measured on the benchmark workloads, the table raises bookkeeping
from about 2.4% to about 8.6% of the arena and adds a few nanoseconds per
operation (up to about 6 ns per allocation and 9 ns per deallocation in the
GCC run below), mostly from an extra cache miss. That is too much to impose
by default, so it is enabled with `PoolConfig::track_requested_bytes = true`
when the statistic is wanted.

## Benchmarks

`nanoalloc_bench` replays nine deterministic allocation traces against
`nanoalloc::Pool` and against the system allocator (`malloc`/`free`), and
reports runtime and memory side by side. It uses only the public API, so it
will run unchanged against future engines.

```bash
./build/release/benchmarks/nanoalloc_bench                 # full run (~20 s)
./build/release/benchmarks/nanoalloc_bench --quick         # smoke run
./build/release/benchmarks/nanoalloc_bench --workload small,medium --iterations 15
./build/release/benchmarks/nanoalloc_bench --csv results.csv --verbose
./build/release/benchmarks/nanoalloc_bench --list          # workload descriptions
```

| Workload | Pattern |
| --- | --- |
| `small` | 16-128 B objects, 20k live, random replacement in batches |
| `medium` | 128-4096 B objects, 4k live, random replacement in batches |
| `mixed` | 80% 16-256 B, 18% 256 B-4 KiB, 2% 4-64 KiB, random replacement |
| `lifo` | 50k objects allocated in sequence, freed in reverse order |
| `random` | random allocation and deallocation batches with random victims |
| `bursts` | bursts of short-lived small objects freed in FIFO order |
| `long_short` | short-lived batches interleaved with long-lived objects |
| `fragmentation` | fill with small objects, free every other one, add larger objects, refill the holes |
| `reuse` | batches of similarly sized (40-56 B) objects allocated and freed repeatedly |

How the numbers are produced:

- Traces are generated from a fixed seed **before** timing starts, so random
  number generation is never measured, and all allocators replay the
  identical operation sequence through the same code.
- Every allocation writes its first 8 bytes and its last byte; every
  deallocation reads them back into a checksum that is printed. The work
  cannot be optimized away and every block is really touched.
- Traces are split into segments that contain only allocations or only
  deallocations, and each segment is timed with one `steady_clock` read, so
  allocation and deallocation throughput are reported separately without
  per-operation timer overhead.
- Each workload runs 1 warmup and 9 measured iterations per allocator;
  medians are reported, with the median absolute deviation as a noise
  estimate. Allocators alternate order between iterations to cancel drift.
  The pool is reused across iterations so that both allocators run on warm
  memory.
- Memory metrics come from a separate untimed replay: peak usable bytes
  (`usable_size` / `malloc_usable_size`), payload slack per allocation, peak
  consumed arena bytes, arena utilization and metadata.
- The arena is sized per workload to at least twice the trace's peak live
  requested bytes and doubled until an untimed replay succeeds, so a small
  arena can never make nanoalloc fail where `malloc` succeeds. Any failure
  during a timed run aborts the benchmark.
- No pass/fail performance thresholds are applied.

### Initial results (v0.1)

GCC 13.3.0, `-O3 -DNDEBUG` (CMake Release), Linux 6.18 x86_64, glibc 2.39,
Intel Xeon @ 2.80 GHz (Cascade Lake), 4 vCPUs in a shared KVM virtual machine.
Default configuration (`track_requested_bytes = false`). Medians of 9
iterations; run-to-run noise on this machine is typically 2-15%, so small
differences are not significant. Full output, including Clang 18 results and
the tracked configuration, is in
[docs/benchmark-results-v0.1.md](docs/benchmark-results-v0.1.md).

| Workload | alloc ns/op<br>system | alloc ns/op<br>nanoalloc | free ns/op<br>system | free ns/op<br>nanoalloc | combined Mops/s<br>system | combined Mops/s<br>nanoalloc |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| small | 12.2 | 11.6 | 20.6 | 31.2 | 61.0 | 46.8 |
| medium | 52.5 | 36.1 | 31.7 | 43.5 | 23.8 | 25.1 |
| mixed | 36.2 | 23.2 | 25.5 | 36.9 | 32.4 | 33.3 |
| lifo | 76.5 | 55.7 | 45.5 | 75.8 | 16.4 | 15.2 |
| random | 42.8 | 31.1 | 33.6 | 43.8 | 26.2 | 26.7 |
| bursts | 22.7 | 22.6 | 22.0 | 40.2 | 44.7 | 31.9 |
| long_short | 17.3 | 20.9 | 21.0 | 37.3 | 52.3 | 34.4 |
| fragmentation | 46.7 | 17.1 | 25.7 | 32.5 | 27.6 | 40.4 |
| reuse | 9.8 | 12.6 | 8.7 | 23.3 | 107.9 | 55.7 |

| Workload | peak requested MiB | peak usable MiB<br>system | peak usable MiB<br>nanoalloc | slack B/alloc<br>system | slack B/alloc<br>nanoalloc | slack % of requested<br>system | slack % of requested<br>nanoalloc |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small | 1.07 | 1.21 | 1.42 | 7.5 | 18.2 | 13.5 | 32.7 |
| medium | 4.78 | 4.81 | 6.34 | 8.5 | 396.8 | 0.7 | 33.3 |
| mixed | 7.21 | 7.27 | 9.49 | 8.1 | 261.0 | 1.0 | 33.0 |
| lifo | 12.07 | 12.43 | 16.10 | 7.5 | 83.5 | 3.0 | 33.2 |
| random | 5.15 | 5.24 | 6.88 | 8.1 | 144.7 | 1.9 | 33.2 |
| bursts | 0.18 | 0.19 | 0.24 | 7.6 | 29.5 | 8.5 | 33.0 |
| long_short | 17.92 | 18.10 | 23.92 | 7.6 | 55.4 | 4.6 | 33.2 |
| fragmentation | 3.68 | 3.85 | 4.90 | 7.7 | 43.8 | 5.8 | 33.1 |
| reuse | 0.01 | 0.01 | 0.02 | 7.1 | 16.0 | 14.7 | 33.3 |

nanoalloc's engine metadata is 2.35-2.53% of the arena in every workload; its
peak consumed bytes equal its peak usable bytes (no headers).

What these measurements say about v0.1:

- **Allocation** is faster than glibc in five workloads in this run (`medium`,
  `mixed`, `lifo`, `random`, `fragmentation`), on par in `small` and `bursts`,
  and slower in `long_short` and `reuse`. The Clang run shows the same picture
  except that `bursts` allocation is slower.
- **Deallocation** is slower than glibc in every workload (about 1.3-2.7x per
  operation). Each buddy free eagerly coalesces, which touches the buddy block
  and its free-list neighbours, whereas glibc defers most of that work in its
  thread caches and fast bins.
- **Memory**: power-of-two rounding costs about 33% of the requested bytes on
  these size distributions (16-400 bytes per allocation), versus 0.7-15%
  (about 8 bytes per allocation) of reported usable slack for glibc. glibc's
  per-chunk headers (8 bytes per allocation) are *not* included in its usable
  figures, and the C runtime does not report them exactly, so consumed memory
  is only compared for nanoalloc.

In short, v0.1 is a correct baseline with competitive allocation speed,
slower frees and the expected power-of-two memory waste. Those last two are
exactly what the next engines must improve. These results come from a single
virtual machine and do not support general claims about nanoalloc relative to
other allocators.

## Testing

`ctest` runs four groups of tests, all without external dependencies:

| Group | Label | Contents |
| --- | --- | --- |
| Public contract | `contract` | `test_pool`, `test_stress`, `test_pmr`, `test_allocator`, `test_no_heap`: alignment (1-4096), boundary sizes (15/16/17, 31/32/33, ... up to 64 KiB + 1), zero-byte and null cases, read/write of the whole usable range, non-overlap, content integrity under unrelated activity, exhaustion and full recovery, large and mixed objects, statistics against an independent model, pmr and Allocator integration with standard containers and non-trivial element types, and a check (via counting replacements of global `operator new`) that no Pool operation allocates after construction |
| Stress | `stress` | Deterministic randomized runs with reproducible seeds (8 scenarios x 4 seeds). Every live allocation is pattern-filled across its usable range and verified throughout the run, not only at the end, together with non-overlap, statistics and `verify_integrity()`. Set `NANOALLOC_STRESS_SEED` and `NANOALLOC_STRESS_SCALE` to reproduce or extend a run |
| Engine | `engine` | `test_buddy`: power-of-two rounding, recursive splitting, buddy identification, merging of valid pairs only, multilevel coalescing, recovery of the original block, free-list and bitmap consistency, non-power-of-two arenas, metadata size |
| Misuse | `misuse` | Double free, foreign, interior and misaligned pointers must end in nanoalloc's diagnostic; with ASan, use-after-free and overflow into a free block must be reported by ASan |

The contract and stress tests make no assumption about power-of-two sizes or
buddy fragmentation and must keep passing when the engine is replaced. The
engine tests are specific to the buddy implementation and will be replaced
with it. The examples run as tests too, and the benchmark has a short smoke
test.

## Design of the v0.1 engine

The engine lives in `src/` and is not part of the public interface.

- **Blocks.** The arena is divided into 16-byte granules. A block of order k
  is 16 * 2^k bytes and starts at a granule index that is a multiple of 2^k,
  so blocks are naturally aligned (the arena itself is 4096-aligned). A
  request is served from the smallest order that holds
  `max(bytes, alignment, 16)`.
- **Free lists.** One doubly linked list per order, with the links stored
  inside the free blocks (two pointers fit in 16 bytes), so any block can be
  unlinked in O(1) when its buddy is released. A 64-bit mask of non-empty
  lists finds the smallest suitable order with a single count-trailing-zeros.
- **Bitmaps instead of headers.** A *free* bit per node at every order tells
  whether a buddy is free (and catches double frees), and a *split* bit per
  internal node marks blocks that are divided. Deallocation recovers a
  block's order by walking up from order 0 until it finds a split parent,
  O(order) bit tests. Together the bitmaps cost about 3 bits per 16-byte
  granule (2.3% of the arena), allocated once at construction.
- **Non-power-of-two capacities** are handled as a forest of power-of-two
  root blocks (one per set bit of the granule count); roots never merge with
  each other, and `largest_free_block` is bounded by the largest root.
- **Misuse detection** reuses the bits the free path reads anyway, so it is
  essentially free and always on.
- **Sanitizer support.** Under AddressSanitizer, every free block is poisoned
  and free-list links are unpoisoned only while the engine accesses them, so
  ASan reports use-after-free and overflows into free memory inside the arena.
- **No system allocation after construction.** The arena (aligned
  `operator new`, typically committed lazily by the OS) and the bitmaps are
  the only allocations; the allocate/deallocate path performs none.

## Limitations

- **Power-of-two internal fragmentation.** A request is rounded up to the next
  power of two: up to 50% waste per allocation, about 33% on typical size
  mixes. This is the property the next engine must remove. Large alignments
  are served by blocks at least as large as the alignment (a 16-byte request
  with 4096-byte alignment uses a 4096-byte block).
- **Eager coalescing** makes deallocation slower than in cache-based
  allocators, especially for churn of small objects.
- **Single-threaded.** No internal synchronization.
- **Fixed capacity.** The arena does not grow; exhaustion is an error by
  design. Pages are not returned to the OS while the pool lives.
- **Largest allocation** is the largest power-of-two root of the arena (for a
  3 MiB arena, 2 MiB).
- **Alignment** above 4096 bytes is not supported.
- **Requested-byte statistics** are opt-in (see
  [Memory accounting](#memory-accounting)).
- **Platforms.** Developed and tested on Linux x86-64 with GCC and Clang. The
  code is written for MSVC and a Windows CI job is provided, but MSVC results
  are not part of the v0.1 measurements. 32-bit targets are supported by
  design but have not been tested.

## Roadmap

Candidate optimizations, to be justified by the benchmarks before they are
adopted:

1. **16-byte granular size classes or segregated fits** (for example a
   TLSF-style two-level index) to bring payload slack under 15 bytes per
   allocation, which is the project's central goal.
2. **Deferred or lazy coalescing** to cut deallocation cost and the
   split/merge churn visible in `reuse` and `bursts`.
3. **A more cache-friendly metadata layout**: interleaving the free and split
   bits of all orders for a group of granules so that a free touches one or
   two cache lines instead of one per level; the XOR-of-buddies trick could
   also halve the free bitmap.
4. **Cheaper requested-size tracking**, so exact slack statistics can be on by
   default.
5. **Thread support** (per-thread pools or caches) once single-thread
   performance is settled.
6. **Returning unused pages** to the OS and growable arenas.

## Project layout

```
include/nanoalloc/   public headers (pool, memory_resource, allocator, version, config)
src/                 Pool implementation and the internal buddy engine
tests/               contract, stress, engine and misuse tests (CTest)
benchmarks/          nanoalloc_bench: workloads, runner and reporting
examples/            runnable usage examples (also run by CTest)
docs/                benchmark results
cmake/               build helpers and package configuration
```

## License

MIT; see [LICENSE](LICENSE).
