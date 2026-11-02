# nanoalloc

**nanoalloc is a C++20 memory allocator library focused on combining high
runtime performance with efficient memory utilization. Its goal is to provide
fast allocation and deallocation while minimizing wasted memory.**

This release (0.1) provides an arena-backed pool built on a binary buddy
allocator, standard-library adapters, statistics, a dependency-free test
suite and a benchmark tool that reports both speed and memory use.

- [Features](#features)
- [Supported platforms](#supported-platforms)
- [Building](#building)
- [Usage](#usage)
- [API reference](#api-reference)
- [Correctness guarantees](#correctness-guarantees)
- [Memory accounting](#memory-accounting)
- [Implementation](#implementation)
- [Testing](#testing)
- [Benchmarking](#benchmarking)
- [Limitations](#limitations)

## Features

- `nanoalloc::Pool`: an arena-backed allocator. One contiguous arena is
  reserved at construction (RAII); allocation and deallocation never call the
  system allocator, and an exhausted arena is reported, never silently
  redirected to `malloc`/`new`.
- 16-byte minimum alignment; any power-of-two alignment up to 4096 bytes.
- Size-free deallocation: `deallocate(ptr)` and `usable_size(ptr)` need only
  the pointer.
- `nanoalloc::MemoryResource`, a `std::pmr::memory_resource` adapter.
- `nanoalloc::Allocator<T>`, a C++20 standard allocator for ordinary STL
  containers.
- `PoolStats`: requested, usable and consumed bytes (live and peak), free
  space, the largest available block, metadata size, and allocation, failure,
  split and merge counters.
- Misuse detection (foreign pointers, pointers into the middle of a block,
  most double frees) and AddressSanitizer integration that poisons free arena
  memory.
- No third-party dependencies.

## Supported platforms

| Platform | Compilers | Verified by |
| --- | --- | --- |
| Linux x86-64 | GCC 13, Clang 18 | Local runs and CI (Debug, Release, ASan + UBSan, shared library) |
| Windows x64 | MSVC 2022 | CI (Debug, Release, ASan) |

Any C++20 compiler with `<memory_resource>` and `<bit>` should work. 32-bit
targets are supported by the code but are not tested.

## Building

Requirements: CMake 3.20 or newer (3.21 for presets) and a C++20 compiler.

With presets (Ninja):

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Without presets (any generator):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
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

The build does not depend on version-control metadata; a plain source copy
builds and tests the same way.

| CMake option | Default | Meaning |
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
  synchronized; concurrent calls are data races. Separate pools may be used
  from separate threads. The pool has no internal locking.

## API reference

All public names are in namespace `nanoalloc`; headers are under
`include/nanoalloc/`. `<nanoalloc/nanoalloc.hpp>` includes everything. The
public headers contain no implementation-specific types.

### PoolConfig

| Member | Default | Meaning |
| --- | --- | --- |
| `capacity_bytes` | 0 (invalid) | Arena size; rounded up to a multiple of 16 |
| `track_requested_bytes` | `false` | Keep exact requested-byte statistics (see [Memory accounting](#memory-accounting)) |

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

### MemoryResource and Allocator&lt;T&gt;

`MemoryResource(Pool&)` implements `std::pmr::memory_resource`. Two resources
compare equal when they refer to the same pool. Allocation failures throw
`std::bad_alloc`; alignments follow the pool's rules.

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

## Correctness guarantees

- **Alignment**: every pointer is aligned to at least 16 bytes and to the
  requested power-of-two alignment (up to 4096).
- **Usable size**: `usable_size(p)` is at least the requested size, and the
  caller may read and write all of it.
- **No overlap**: the usable ranges of live allocations never overlap.
- **Zero-byte requests** return a unique, non-null, 16-byte aligned pointer
  that must be released like any other allocation.
- **Failure** leaves the pool unchanged apart from
  `PoolStats::failed_allocation_count` (strong exception guarantee).
- **No fallback**: when the arena cannot satisfy a request, the request fails.
- **No allocation after construction**: allocate and deallocate never call the
  global allocation functions or the C allocator.
- **Recovery**: once every allocation has been released, `free_bytes` equals
  `capacity_bytes` and `largest_free_block` returns to its initial value.
- **Misuse** (a pointer not returned by this pool, already released, or into
  the middle of a block) is undefined behavior. The pool detects the common
  cases and terminates with a `nanoalloc: fatal error: ...` diagnostic instead
  of corrupting its state.

## Memory accounting

`PoolStats` distinguishes four quantities:

| Term | Meaning |
| --- | --- |
| requested | the `bytes` argument passed to `allocate` |
| usable | what `usable_size()` reports; the caller may use all of it (>= requested) |
| consumed | arena bytes made unavailable by an allocation, including any in-arena per-allocation overhead (>= usable; equal in this implementation, which stores no per-allocation header) |
| reserved | memory obtained from the system: the arena (`capacity_bytes`) plus bookkeeping (`metadata_bytes`, `tracking_bytes`) |

*Payload slack* is usable − requested. *Fragmentation* is free arena memory
that cannot serve a request because it is not contiguous; compare
`free_bytes` with `largest_free_block`.

| Field | Meaning |
| --- | --- |
| `capacity_bytes` | Arena size |
| `metadata_bytes` | Allocator bookkeeping held outside the arena (allocated at construction) |
| `tracking_bytes` | Requested-size table; 0 unless tracking is enabled |
| `live_allocations` | Allocations not yet released |
| `live_requested_bytes`, `peak_requested_bytes` | Sum of live requested sizes, and its maximum; 0 unless tracking is enabled |
| `live_usable_bytes`, `peak_usable_bytes` | Sum of live usable sizes, and its maximum |
| `live_consumed_bytes`, `peak_consumed_bytes` | Arena bytes consumed by live allocations, and its maximum |
| `free_bytes` | `capacity_bytes - live_consumed_bytes` (not necessarily contiguous) |
| `largest_free_block` | Any request with `bytes <= largest_free_block` and alignment <= 16 succeeds now |
| `allocation_count`, `deallocation_count` | Successful allocations; deallocations of non-null pointers |
| `failed_allocation_count` | `allocate`/`try_allocate` calls that returned no memory |
| `split_count`, `merge_count` | Block divisions and coalescings performed by the allocator |
| `requested_bytes_tracked` | Whether the requested-byte fields are meaningful |

All values are exact. Peaks are tracked independently and may have been
reached at different moments.

**Requested-byte tracking** is optional. Because `deallocate()` receives no
size, exact requested-byte statistics need a side table with one byte per
16-byte granule of the arena (reported as `tracking_bytes`) and one table
access per allocation and deallocation. Enable it with
`PoolConfig::track_requested_bytes = true`.

## Implementation

The allocator is a binary buddy system; its code is in `src/` and is not part
of the public interface.

- **Blocks.** The arena is divided into 16-byte granules. A block of order k
  is 16 * 2^k bytes and starts at a granule index that is a multiple of 2^k,
  so blocks are naturally aligned (the arena itself is 4096-aligned). A
  request is served from the smallest order that holds
  `max(bytes, alignment, 16)`; a larger free block is split in halves as
  needed.
- **Release.** A released block is merged with its buddy whenever the buddy
  is free, repeatedly, up to the largest possible block.
- **Free lists.** One doubly linked list per order, with the links stored
  inside the free blocks.
- **Metadata.** Blocks carry no header. A free bit per node at every order and
  a split bit per internal node record the block structure; the block size of
  a pointer is recovered from them on release. Together they cost about
  3 bits per 16-byte granule (about 2.3% of the arena), allocated once at
  construction.
- **Arena sizes that are not a power of two** are handled as a set of
  power-of-two root blocks (one per set bit of the granule count). Roots do
  not merge with each other, so `largest_free_block` is bounded by the largest
  root.
- **Sanitizer support.** Under AddressSanitizer, free blocks are poisoned, so
  ASan reports use-after-free and overflows into free memory inside the arena.

Characteristics that follow from this design:

- The usable size of an allocation is the smallest power of two that is at
  least `max(bytes, alignment, 16)`.
- Allocation splits at most one block per level; deallocation inspects one
  bitmap bit per level of the released block and merges at most once per
  level. The number of levels is log2(capacity / 16).
- Memory outside the arena is limited to the metadata above (and the
  requested-size table when enabled).

## Testing

`ctest` runs four groups of tests, all without external dependencies:

| Group | Label | Contents |
| --- | --- | --- |
| Public contract | `contract` | `test_pool`, `test_stress`, `test_pmr`, `test_allocator`, `test_no_heap`: alignment (1-4096), boundary sizes (15/16/17, 31/32/33, ... up to 64 KiB + 1), zero-byte and null cases, read/write of the whole usable range, non-overlap, content integrity under unrelated activity, exhaustion and full recovery, large and mixed objects, statistics against an independent model, pmr and Allocator integration with standard containers and non-trivial element types, and a check (via counting replacements of global `operator new`) that no Pool operation allocates after construction |
| Stress | `stress` | Deterministic randomized runs with reproducible seeds (8 scenarios x 4 seeds). Every live allocation is pattern-filled across its usable range and verified throughout the run, not only at the end, together with non-overlap, statistics and `verify_integrity()`. Set `NANOALLOC_STRESS_SEED` and `NANOALLOC_STRESS_SCALE` to reproduce or extend a run |
| Implementation | `engine` | `test_buddy`: power-of-two rounding, recursive splitting, buddy identification, merging of valid pairs only, multilevel coalescing, recovery of the original block, free-list and bitmap consistency, non-power-of-two arenas, metadata size |
| Misuse | `misuse` | Double free, foreign, interior and misaligned pointers must end in nanoalloc's diagnostic; with ASan, use-after-free and overflow into a free block must be reported by ASan |

The contract and stress tests use only the public API and do not depend on
implementation details such as power-of-two block sizes. The implementation
tests exercise the internal buddy allocator directly. The examples run as
tests too, and the benchmark has a short smoke test.

## Benchmarking

`nanoalloc_bench` replays nine deterministic allocation traces against
`nanoalloc::Pool` and against the system allocator (`malloc`/`free`), and
reports runtime and memory side by side. It uses only the public API.

```bash
./build/release/benchmarks/nanoalloc_bench                 # full run
./build/release/benchmarks/nanoalloc_bench --quick         # short run
./build/release/benchmarks/nanoalloc_bench --workload small,medium --iterations 15
./build/release/benchmarks/nanoalloc_bench --csv results.csv --verbose
./build/release/benchmarks/nanoalloc_bench --list          # workload descriptions
./build/release/benchmarks/nanoalloc_bench --help
```

Use a Release build; the tool prints a warning when it was built without
optimization or with sanitizers.

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

Method:

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
- Each workload runs 1 warmup and 9 measured iterations per allocator by
  default; medians are reported, with the median absolute deviation as a
  noise estimate. Allocators alternate order between iterations. The pool is
  reused across iterations so that both allocators run on warm memory.
- Memory metrics come from a separate untimed replay: peak usable bytes
  (`usable_size` / `malloc_usable_size`), payload slack per allocation, peak
  consumed arena bytes, arena utilization and metadata.
- The arena is sized per workload to at least twice the trace's peak live
  requested bytes and doubled until an untimed replay succeeds, so a small
  arena can never make nanoalloc fail where `malloc` succeeds. Any failure
  during a timed run aborts the benchmark.
- Allocators: `system` (`malloc`/`free`), `nanoalloc` (default `PoolConfig`)
  and `nanoalloc-tracked` (`track_requested_bytes = true`).

The report ends with a definition of every column. Results depend on the
hardware, compiler and system allocator, so measure on the machine you care
about.

## Limitations

- **Power-of-two rounding.** The usable size of an allocation is rounded up to
  a power of two, so an allocation can use up to about twice the requested
  memory. Large alignments are served by blocks at least as large as the
  alignment (a 16-byte request with 4096-byte alignment uses a 4096-byte
  block).
- **Single-threaded.** A pool has no internal synchronization.
- **Fixed capacity.** The arena does not grow; exhaustion is an error by
  design. Memory is not returned to the operating system while the pool
  lives.
- **Largest allocation** is the largest power-of-two root of the arena (for a
  3 MiB arena, 2 MiB).
- **Alignment** above 4096 bytes is not supported.
- **Requested-byte statistics** are opt-in (see
  [Memory accounting](#memory-accounting)).

## Project layout

```
include/nanoalloc/   public headers (pool, memory_resource, allocator, version, config)
src/                 Pool implementation and the internal buddy allocator
tests/               contract, stress, implementation and misuse tests (CTest)
benchmarks/          nanoalloc_bench: workloads, runner and reporting
examples/            runnable usage examples (also run by CTest)
cmake/               build helpers and package configuration
```

## License

MIT; see [LICENSE](LICENSE).
