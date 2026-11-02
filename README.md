# nanoalloc

**nanoalloc is a C++20 memory allocator library that aims to combine memory
efficiency with runtime efficiency: fast allocation and deallocation, from
many threads, with no more than 15 bytes of unused payload per allocation.**

- [Features](#features)
- [Supported platforms](#supported-platforms)
- [Building](#building)
- [Usage](#usage)
- [Threads](#threads)
- [How it works](#how-it-works)
- [Guarantees](#guarantees)
- [API reference](#api-reference)
- [Memory accounting](#memory-accounting)
- [Performance](#performance)
- [Testing](#testing)
- [Limitations](#limitations)

## Features

- `nanoalloc::Pool`: a capacity-bounded memory pool. Each thread allocates
  from its own spans without locks; any thread may free any block, also after
  the thread that allocated it has exited.
- **16-byte granularity**: a request of `n` bytes gets exactly `n` rounded up
  to a multiple of 16 usable bytes (at most 15 bytes of slack), 16-byte
  aligned, plus a 16-byte header. Power-of-two alignments up to 4096 bytes are
  supported without changing the payload size.
- **Immediate coalescing**: a freed block is merged with free neighbours at
  once, so free memory is always made of maximal blocks.
- **Lock-free cross-thread frees**: a block freed by another thread is queued
  in its owner's inbox and released by the owner.
- Size-free deallocation: `deallocate(ptr)` and `usable_size(ptr)` need only
  the pointer.
- `nanoalloc::MemoryResource` (`std::pmr::memory_resource`) and
  `nanoalloc::Allocator<T>` (standard C++20 allocator) adapters.
- Exact statistics of requested, usable and consumed bytes, headers, spans,
  pending remote frees, splits and merges, from per-thread counters.
- Misuse detection (foreign, misaligned and interior pointers, double frees)
  and AddressSanitizer poisoning of free memory.
- No third-party dependencies.

## Supported platforms

| Platform | Compilers | Verified by |
| --- | --- | --- |
| Linux x86-64 | GCC 13, Clang 18 | Local runs and CI: Debug, Release, ASan + UBSan, TSan, shared library |
| Windows x64 | MSVC 2022 | CI: Debug, Release, ASan |

The code uses only standard C++20 (`<atomic>`, `<bit>`, `thread_local`) and
should build on other 64-bit platforms such as ARM64, but only the platforms
above are tested; 32-bit targets compile but are not tested.

## Building

Requirements: CMake 3.20 or newer (3.21 for presets) and a C++20 compiler.

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Without presets:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizers (GCC or Clang; TSan uses Clang):

```bash
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

On Windows, `cmake --preset msvc`, then `cmake --build --preset msvc-release`
and `ctest --preset msvc-release` (the `msvc-asan` presets enable ASan).

The build does not depend on version-control metadata. CMake options:
`NANOALLOC_BUILD_TESTS`, `NANOALLOC_BUILD_BENCHMARKS`,
`NANOALLOC_BUILD_EXAMPLES` (all ON at top level), `NANOALLOC_SANITIZE`
(e.g. `address;undefined` or `thread`), `NANOALLOC_WARNINGS_AS_ERRORS`,
`BUILD_SHARED_LIBS`. The installed package is used with
`find_package(nanoalloc 0.1 REQUIRED)` and `nanoalloc::nanoalloc`.

## Usage

### Pool

```cpp
#include <nanoalloc/pool.hpp>

#include <cstdio>

int main() {
    nanoalloc::Pool pool({64 * 1024 * 1024});  // 64 MiB budget

    void* data = pool.allocate(100);      // 112 usable bytes, 16-byte aligned
    void* line = pool.allocate(64, 64);   // 64 usable bytes, 64-byte aligned

    std::printf("usable: %zu bytes\n", pool.usable_size(data));

    pool.deallocate(line);  // no size needed
    pool.deallocate(data);

    const nanoalloc::PoolStats stats = pool.stats();
    std::printf("live: %zu, reserved: %zu of %zu bytes\n", stats.live_allocations,
                stats.reserved_bytes, stats.capacity_bytes);
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

Runnable versions are in [`examples/`](examples) and are executed by the test
suite. The pool must outlive every allocation, adapter and container that
uses it; destroying the pool releases all of its memory at once without
running destructors. A `Pool` is neither copyable nor movable.

## Threads

Allocation, deallocation, `usable_size()`, `owns()`, `stats()` and
`drain_pending_frees()` may be called from any number of threads at the same
time. `verify_integrity()` requires that no other thread uses the pool.

**Cross-thread frees.** A thread may free a block allocated by another
thread. The block is queued in the owner's inbox; the owner releases it (and
merges it with its neighbours) at its next allocation or deallocation in that
pool, or when it calls `pool.drain_pending_frees()`. Until then the block
still counts as live memory (`PoolStats::pending_remote_frees`). A thread that
frees memory for others but rarely allocates itself can call
`drain_pending_frees()` to return memory promptly. Running threads get no
background processing; only the heaps of exited threads do (below).

**Allocations outlive their thread.** A block stays valid until it is
deallocated or the pool is destroyed, whether or not the thread that
allocated it is still running. Any thread may read, write and deallocate it,
with the synchronization any shared object needs.

When a thread that used the pool terminates (as its `thread_local` objects are
destroyed), the pool releases the frees waiting in the thread's heap and
returns its completely free spans to the pool. Then:

- A heap that still holds allocated blocks is **retired**: handed to a
  process-wide reclamation worker (one background thread, started the first
  time a heap is retired). Frees of its blocks, from any thread, go to its
  inbox like other cross-thread frees. The worker processes them
  asynchronously: it polls every 1 ms while it finds work and backs off to
  every 50 ms when idle, so memory freed into a retired heap is reclaimed
  within about 50 ms. Spans that become empty go back to the pool for other
  threads, and a heap that holds nothing becomes reusable.
  `PoolStats::retired_thread_heap_count` counts the retired heaps.
- **No allocation after retirement.** In `thread_local` destructors that run
  after this point, `allocate()` throws `std::bad_alloc` and `try_allocate()`
  returns `nullptr`, for every pool; the thread gets no new heap. Its
  deallocations are accepted and go through the owners' inboxes.
- A heap that holds no block is reused by the next new thread, together with
  its statistics.

## How it works

**Spans and thread heaps.** At construction the pool reserves its budget as
one arena (pages are typically committed lazily by the operating system).
The first time a thread allocates from a pool it gets a *thread heap* for
that pool, found through a small `thread_local` cache, and on demand the
thread heap acquires 1 MiB *spans* from the arena under a short pool-wide
lock. A thread keeps the spans it acquired while it runs.

**Blocks.** A span is divided into variable-sized blocks, each a 16-byte
header followed by its payload; sizes are multiples of 16. The header holds:

- word 0: the owning thread heap and the block state (allocated, free, or
  pending a remote free); for a pending block it links the block into the
  owner's inbox instead;
- word 1: the block size, the size of the previous block in the span, the
  payload slack (so requested sizes are known exactly) and a check value.

A block's physical neighbours are found from its own size and the previous
size, so freeing and merging never search. Spans start with a small header
and end with a permanently allocated fence block, so blocks never merge
across span boundaries.

**Free buckets.** Every free block of a thread's spans is on one of 384
doubly linked lists by size: 256 buckets of 16-byte steps up to 4 KiB, then
16 buckets per power of two up to 1 MiB. A 6-word bitmap marks non-empty
buckets and a summary word marks non-empty bitmap words, so the smallest
suitable non-empty bucket is found with two count-trailing-zeros operations.
Buckets above 4 KiB hold a range of sizes; the search takes such a bucket's
first block only if it is large enough, otherwise it moves to larger buckets,
and only then examines the rest of the bucket, so an undersized block is
never returned.

**Allocation.** A request for `n` bytes needs a block of `round_up(n, 16) +
16` bytes. If the exact-size bucket has a block, it is taken whole. Otherwise
the smallest suitable block is split: the allocation is carved from its end,
so the remaining free block keeps its header and often its bucket. A
remainder of exactly 16 bytes cannot hold list links; it stays a header-only
free *fragment* that is merged as soon as a neighbour is freed, so the
allocation never receives more than its rounded payload. Larger alignments
place the block at an aligned position inside a free block and keep the
pieces before and after it free.

**Deallocation.** A local free merges the block with free neighbours
immediately and puts the result in its bucket. A remote free changes the
block's state to pending with one compare-and-swap and pushes it onto the
owner's inbox with a second one; the owner later detaches the whole inbox
with one atomic exchange and frees the blocks normally.

**Oversized requests.** Requests that do not fit in a span (more than
1 MiB - 80 bytes, or a smaller request with a large alignment) are served by
the system allocator with the same 16-byte header in front, are charged
against the pool's budget, and are released by `deallocate()` as usual.

**Retirement.** A thread's exit hook (a `thread_local` object) finishes the
thread's own work on its heap under the pool lock, then clears the thread's
cached heap pointers, so later calls of that thread find no heap. A heap with
live blocks is then linked into the reclamation worker's list under the
worker's mutex, which orders everything the owner did before anything the
worker does. Remote frees need no change: they always push to the owner's
inbox, whether the owner runs or not. The worker drains retired inboxes with
the normal release-and-coalesce path, and a heap is reusable only when it
has no span left, i.e. no allocated or pending block that a freeing thread
could still be working on. See [docs/THREAD_RETIREMENT.md](docs/THREAD_RETIREMENT.md).

## Guarantees

- Every pointer is aligned to at least 16 bytes and to the requested
  power-of-two alignment (up to 4096).
- Span allocations have exactly `round_up(n, 16)` usable bytes: at most 15
  bytes of payload slack. The 16-byte header is reported separately as
  allocator overhead.
- The usable ranges of live allocations never overlap.
- Zero-byte requests return a unique non-null pointer (a header-only block)
  that must be released like any other allocation.
- A failed allocation leaves the pool unchanged apart from
  `failed_allocation_count` (and possibly a newly created, empty thread
  heap).
- Allocation and deallocation never call the system allocator except for
  oversized requests; a thread's first allocation creates its heap inside the
  arena.
- Everything the pool hands out is charged against its budget: thread heaps,
  spans and oversized allocations together never exceed `capacity_bytes`
  (`reserved_bytes <= capacity_bytes`), and the pool never falls back to
  another allocator. The budget is not a limit on the memory obtained from the
  operating system: the arena is allocated in full at construction, and
  oversized allocations are separate system allocations on top of it
  (`system_reserved_bytes`, at most about twice the budget).
- In single-threaded use, once everything has been freed `free_bytes` equals
  `capacity_bytes` and `largest_free_block` returns to its initial value.
- Misuse (a pointer this pool did not return, an interior pointer, a double
  free) is undefined behaviour; the common cases are detected and terminate
  the process with a `nanoalloc: fatal error: ...` message.

## API reference

All names are in namespace `nanoalloc`; `<nanoalloc/nanoalloc.hpp>` includes
everything.

| `PoolConfig` member | Default | Meaning |
| --- | --- | --- |
| `capacity_bytes` | 0 (invalid) | Memory budget for thread heaps, spans and oversized allocations; also the arena size |
| `track_requested_bytes` | `true` | Report requested-byte statistics (free; `false` reports them as zero) |

| `Pool` member | Behaviour |
| --- | --- |
| `explicit Pool(const PoolConfig&)` | Reserves the arena. `std::invalid_argument` for capacity 0 or above `max_capacity`; `std::bad_alloc` if the memory cannot be obtained |
| `void* allocate(bytes, alignment = 16)` | `std::invalid_argument` for a zero or non-power-of-two alignment; `std::bad_alloc` if the request cannot be satisfied or the alignment exceeds 4096 |
| `void* try_allocate(bytes, alignment = 16) noexcept` | As `allocate`, `nullptr` on any failure |
| `void deallocate(void* ptr) noexcept` | Releases `ptr` from any thread; `nullptr` is a no-op |
| `void drain_pending_frees() noexcept` | Releases blocks other threads freed on behalf of the calling thread |
| `size_t usable_size(const void* ptr) const noexcept` | Usable bytes at `ptr`; 0 for `nullptr` |
| `bool owns(const void* ptr) const noexcept` | Whether `ptr` lies in the arena or in a live oversized allocation |
| `size_t capacity() const noexcept` | The budget |
| `PoolStats stats() const noexcept` | Statistics snapshot (takes the pool lock) |
| `bool verify_integrity() const noexcept` | Full consistency check; no concurrent use allowed |
| `min_alignment`, `max_alignment`, `granularity`, `header_size`, `max_capacity` | 16, 4096, 16, 16, 2^62 |

`MemoryResource(Pool&)` implements `std::pmr::memory_resource` (equal when it
refers to the same pool). `Allocator<T>(Pool&)` is a one-pointer C++20
allocator: rebinding keeps the pool, allocators compare equal iff their pools
are the same, it propagates on copy assignment, move assignment and swap, and
storage is aligned to `max(alignof(T), 16)`.

**Changes from release 0.1** (the buddy allocator): pools are now usable from
many threads; `capacity_bytes` is a budget that also covers oversized
allocations served by the system allocator; requested-byte tracking is on by
default and free; `PoolStats` gained reservation, span, thread-heap, header,
inbox, fragment and oversized-allocation fields; `metadata_bytes` now counts
bookkeeping carved from the arena; `drain_pending_frees()` is new.

## Memory accounting

| Term | Meaning |
| --- | --- |
| requested | the `bytes` passed to `allocate` |
| usable | `usable_size()`: `round_up(bytes, 16)` for span blocks |
| consumed | usable bytes plus the 16-byte header (span blocks), or the whole system allocation (oversized) |
| reserved | charged against the budget: arena carved for thread heaps and spans, plus oversized system memory |

`PoolStats` separates the budget, the memory obtained from the system and
where each reserved byte is:

| Field | Meaning |
| --- | --- |
| `capacity_bytes` | the budget |
| `reserved_bytes` | budget charged so far; never above `capacity_bytes` |
| `system_reserved_bytes` | memory obtained from the system: the whole arena, the pool object and live oversized allocations |
| `metadata_bytes` | thread heap objects and the headers and fences of spans owned by threads |
| `live_consumed_bytes` | live allocations, headers included (also `live_usable_bytes`, `live_requested_bytes`, `header_bytes`) |
| `thread_free_bytes` | free blocks inside spans owned by threads (only the owner allocates from them); includes `fragment_bytes` |
| `free_span_count` / `free_span_bytes` | completely free spans returned by exited threads, available to any thread |
| `span_count` / `span_bytes` | all spans carved from the arena |
| `oversized_live_allocations` / `oversized_live_bytes` / `oversized_reserved_bytes` | live oversized blocks: count, payload and system bytes |
| `pending_remote_frees` | blocks freed by another thread and still waiting in their owner's inbox (they count as live) |
| `thread_heap_count` / `active_thread_heap_count` / `retired_thread_heap_count` | thread heaps carved; those owned by a running thread; those of exited threads that still hold blocks (managed by the reclamation worker) |
| `free_bytes` | `capacity_bytes - live_consumed_bytes`: a utilization figure, not memory the caller can allocate |
| `largest_free_block` | the largest request the calling thread is guaranteed to get from a span now |

When no other thread uses the pool during `stats()`:

```
reserved_bytes   == metadata_bytes + live_consumed_bytes + thread_free_bytes + free_span_bytes
reserved_bytes   <= capacity_bytes
allocation_count == deallocation_count + live_allocations - pending_remote_frees
```

*Payload slack* is usable − requested (0-15 bytes per span allocation). *Total
allocator overhead* adds the 16-byte headers (`header_bytes`), bookkeeping
(`metadata_bytes`) and free memory held in spans (`thread_free_bytes`,
`free_span_bytes`).

Counters cover the whole life of the pool: allocation, deallocation (local,
remote and oversized), failure, split and merge counts, and peaks of
requested, usable and consumed bytes. A remote free is counted once, by the
heap that receives it, so `pending_remote_frees` is exactly the work waiting
in inboxes. A thread heap reused by a new thread keeps its counters, so
nothing is lost when threads come and go.

Every counter is updated by the thread that owns it (no shared atomic
counter on the allocation path) and read with relaxed atomic loads. Values
are exact when no other thread uses the pool during `stats()`; under
concurrent use each value is recent but fields may come from slightly
different moments. Peaks are the sum of per-heap peaks (plus one for
oversized blocks): exact when one thread allocates from spans only, an upper
bound otherwise. They never decrease.

## Performance

Benchmark programs use only the public API:

- `nanoalloc_bench`: nine deterministic single-thread traces (small, medium
  and mixed objects, LIFO, random, bursts, long- and short-lived objects,
  fragmentation, reuse) against the system allocator. It reports
  allocation and deallocation throughput with their spread, and, from a
  separate untimed replay, memory (usable, consumed and reserved bytes,
  slack, metadata). Every replay must reproduce a checksum computed from the
  trace, and the allocator order rotates between iterations.
- `nanoalloc_mtbench`: 1-16 threads with 0-100% cross-thread frees, and a
  many-to-one inbox scenario. Deallocation is reported both as issued
  (`free`) and as completed, including the owner's drain (`done`).
- `nanoalloc_paired` (built when `NANOALLOC_REFERENCE_SOURCE_DIR` names
  another nanoalloc source tree): runs this version and the other one in
  the same process, alternating every iteration, and reports time ratios.
  This resolves differences of a few percent on machines whose speed drifts
  much more between runs.

```bash
./build/release/benchmarks/nanoalloc_bench
./build/release/benchmarks/nanoalloc_mtbench
```

Single-thread throughput against glibc 2.39 on a 4-vCPU Intel Xeon (Cascade
Lake) virtual machine, GCC 13.3 `-O3` (median of three runs), and the time
ratio against the previous buddy allocator measured in the same process
(below 1 is faster; Zen 3 numbers from GitHub-hosted runners). The full
tables, method, profiles and the optimization history are in
[docs/PERFORMANCE.md](docs/PERFORMANCE.md).

| Workload | combined Mops/s glibc | nanoalloc | time vs buddy, Intel GCC | Zen 3 GCC | Zen 3 MSVC | slack B/alloc buddy | nanoalloc |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small | 63.1 | 61.6 | 0.87 | 0.93 | 0.94 | 18.2 | 7.5 |
| medium | 24.6 | 29.6 | 0.90 | 1.15 | 1.14 | 396.8 | 7.5 |
| mixed | 30.6 | 41.7 | 0.78 | 0.92 | 0.98 | 261.0 | 7.5 |
| lifo | 17.1 | 47.8 | 0.38 | 0.47 | 0.53 | 83.5 | 7.5 |
| random | 27.0 | 42.5 | 0.68 | 0.87 | 0.93 | 144.7 | 7.5 |
| bursts | 62.1 | 68.7 | 0.50 | 0.61 | 0.72 | 29.5 | 7.5 |
| long_short | 65.5 | 41.1 | 1.03 | 1.03 | 1.11 | 55.4 | 7.5 |
| fragmentation | 29.2 | 43.2 | 0.92 | 0.99 | 1.06 | 43.8 | 7.5 |
| reuse | 95.1 | 52.3 | 1.01 | 1.11 | 1.20 | 16.0 | 7.5 |

With four threads, aggregate combined throughput was 141 vs 76 Mops/s
(glibc) without cross-thread frees and 45 vs 18 Mops/s with 25% of blocks
freed by another thread. These results come from shared virtual machines
and a specific set of workloads, and the ranking depends on the CPU. glibc
is faster on workloads that repeatedly allocate and free the same sizes
(`reuse`, `long_short`), where it reuses cached chunks while nanoalloc
splits and merges blocks every time; the buddy allocator, with
power-of-two classes, is faster there on some CPUs.

## Testing

`ctest` runs, without external dependencies:

| Group | Label | Contents |
| --- | --- | --- |
| Public contract | `contract` | alignment (1-4096), boundary sizes, zero-byte and null cases, read/write of the whole usable range, non-overlap, content integrity, exhaustion and recovery, large and oversized objects, statistics against a model, pmr and Allocator with standard containers, no system allocation during operations (`test_no_heap`) |
| Stress | `stress` | deterministic randomized runs (8 scenarios x 4 seeds) verifying data patterns, non-overlap, statistics and `verify_integrity()` throughout; `NANOALLOC_STRESS_SEED`/`NANOALLOC_STRESS_SCALE` reproduce or extend a run |
| Threads | `threads` | cross-thread frees, many threads freeing into one owner, explicit and implicit draining, concurrent frees while the owner allocates and drains, all-to-all exchange, blocks shared during and after their owner's life, span reuse after thread exit, statistics across heap reuse and thread churn (with concurrent remote frees), concurrent statistics |
| Retirement | `threads` | blocks freed after their thread exited, concurrent frees into several retired heaps, the last free racing the worker, `thread_local` destructors that free and that try to allocate after retirement, span and heap reuse, 2000 short-lived threads with bounded heaps and spans, pool destruction during reclamation, statistics, randomized exit and free timing (`test_retirement`) |
| Engine | `engine` | exact 16-byte rounding, header accounting, bucket layout, splitting at every small remainder, 16-byte fragments, immediate and multilevel coalescing, bitmap transitions, coarse-bucket search, span acquisition and boundaries, oversized allocations, several pools per thread, randomized fragmentation with full validation |
| Misuse | `misuse` | double frees, foreign, misaligned and interior pointers, other pool; under ASan, use-after-free and overflow into free memory |

Contract, stress, thread and retirement tests use only the public API. Run the
`sanitize` (ASan + UBSan) and `tsan` presets to check memory and thread
safety; under TSan `test_no_heap` is not built because it replaces the global
`operator new`, which the TSan runtime also defines.

## Limitations

- **Memory retention per thread.** A thread keeps the spans it acquired until
  it exits, even when they are empty, so reserved memory grows with the number
  of threads (about one or more 1 MiB spans per thread). Memory is never
  returned to the operating system while the pool lives.
- **Retired heaps.** Blocks a thread leaves allocated when it exits keep their
  spans (and their heap) until they are freed; memory freed into a retired
  heap is reclaimed asynchronously, within about 50 ms. A thread may not
  allocate from `thread_local` destructors that run after its exit has been
  processed (see [Threads](#threads)).
- **Carved spans stay charged.** Spans are never returned to the budget while
  the pool lives (free ones are reused by other threads), so a pool whose
  arena has been carved into spans cannot serve oversized requests any more.
- **System memory above the budget.** The arena is allocated in full at
  construction; oversized allocations are additional system allocations, so
  the memory obtained from the system can approach twice the budget.
- **Remote frees are deferred**: a block freed by another thread is reused
  only after its owner allocates, frees or drains.
- **16-byte header** per allocation: for very small objects the header can
  exceed the rounding savings.
- **Fixed budget**: the pool does not grow beyond `capacity_bytes`. A pool
  smaller than 1 MiB carves spans smaller than 1 MiB, which limits the
  largest span allocation, and each thread's first allocation takes about
  3.4 KiB of the budget for its heap.
- **Alignment** above 4096 bytes is not supported.

## Project layout

```
include/nanoalloc/   public headers
src/                 pool, thread heaps (heap.*), block format (block.hpp)
tests/               contract, stress, thread, engine and misuse tests
benchmarks/          nanoalloc_bench, nanoalloc_mtbench, nanoalloc_paired, compare_builds.py
examples/            runnable usage examples
docs/                performance log, review follow-up report, thread retirement
```

## License

MIT; see [LICENSE](LICENSE).
