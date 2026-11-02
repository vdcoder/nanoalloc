# Review follow-up: correctness, accounting and performance

This report covers the changes made on `feature/fast-furious-16b` in
response to the review of commit `05f2ee7`: the statistics defect when
thread heaps are reused, the thread lifetime contract, capacity and
reservation accounting, the benchmark method, and the `long_short`,
`fragmentation` and `reuse` regressions. Performance details are in
[PERFORMANCE.md](PERFORMANCE.md).

The architecture is unchanged:
- variable-size blocks with 16-byte headers and exact 16-byte granularity
  (at most 15 bytes of slack);
- thread-owned spans of about 1 MiB;
- 384 bitmap-indexed segregated free lists with immediate coalescing;
- one remote-free inbox per owner, drained in batches;
- no slab allocator, background work or span migration.

The block chosen for every request is the same as before, so memory use is
identical.

## 1. Statistics across heap reuse (critical)

**Root cause.** When a thread exits and its heap holds no span, the heap
object is reused by the next new thread. The reuse destroyed the
`ThreadHeap` and constructed a new one in place, which reset all of its
counters. This had two effects:

- Totals and peaks lost the previous thread's history.
- Remote frees were counted on the *sending* heap, and pending frees were
  computed as Σ sent − Σ drained. When a sender's heap was reused, its
  "sent" count vanished, so `pending_remote_frees` and
  `remote_deallocation_count` decreased although the frees were still
  waiting in their owner's inbox.

Reproduced on `05f2ee7` and fixed on this branch:

| Scenario | 05f2ee7 | now |
| --- | --- | --- |
| A: worker A allocates and frees 1000 B, exits; worker B reuses A's heap and allocates and frees 100 B | allocations/deallocations 1/1, peak usable 112 | 2/2, peak usable 1008 |
| B: main allocates X; worker A allocates and frees Y, frees X remotely, exits; worker B reuses A's heap | pending 1 → 0, remote 1 → 0 after B | pending 1, remote 1 (until main drains) |

**Fix.**
- A heap left by an exited thread is *adopted* in place by the next new
  thread. Only its thread id and status change, under the pool lock;
  counters and peaks are kept, so every statistic covers the pool's
  lifetime.
- Remote frees are counted once, by the **receiving** heap. A sender
  increments an atomic counter on the owner's inbox cache line before it
  publishes the block, so the count never trails the owner's drain count.
  `pending_remote_frees` is Σ(received − drained) over all heaps, which is
  exactly the work waiting in inboxes, and `remote_deallocation_count` is
  Σ received. The sender-side and "heapless" counters are gone.
- Peaks are per-heap high-water marks summed over heaps: exact when one
  thread allocates from spans only, an upper bound otherwise, and never
  decreasing. A reused heap keeps its peak.
- `verify_integrity()` checks that received − drained equals the number of
  pending blocks in each heap's spans, that a reusable heap holds nothing,
  and that the carved arena equals heap objects plus spans.

**Hot-path cost.** The remote free path does one extra atomic increment on
a cache line it already owns for the inbox push. A multithreaded A/B
(many-to-one, 4 threads, 8 alternating runs) showed no difference beyond
run-to-run noise.

**Regression tests** (`tests/test_threads.cpp`):
- `recycled_heap_keeps_allocation_totals_and_peaks` (scenario A, exact
  peaks).
- `recycled_heap_keeps_pending_remote_frees` (scenario B, before and after
  draining).
- `sequential_thread_churn_reuses_one_heap_and_keeps_totals`: 200 threads
  one after another; one heap, exact totals and peaks, monotonic history.
- `thread_churn_with_concurrent_remote_frees_keeps_exact_totals`: 25
  rounds of short-lived workers that free the owner's blocks while it
  drains concurrently; exact totals after every round, bounded heap count.

All statistics tests also check the documented identities
(`tests/test_stats.hpp`).

## 2. Thread lifetime contract

The contract, now worded the same way in `pool.hpp`, the
`MemoryResource` and `Allocator` documentation, and the README:

> The thread that allocates a block owns it. All accesses to a block by
> other threads, including its deallocation, must complete before its owner
> begins to terminate.

The adapters were documented as "not thread-safe, like the Pool"; they now
say they are as thread-safe as the Pool, subject to the contract.

**Tests revised.**
- `thread_exit_with_live_blocks_keeps_them_valid` read blocks after their
  owner had exited, which the contract forbids. It is replaced by
  `blocks_are_shared_while_their_owner_runs`: another thread reads and frees
  blocks while the owner waits. Blocks left after the owner exits are not
  touched again.
- `all_threads_exchange_and_free_each_others_blocks` used to leave blocks of
  exited threads in the exchange. Now every thread empties the exchange
  before any thread exits, and the test checks that nothing remains
  allocated.
- New: pmr and `Allocator` containers built by one thread and destroyed by
  another while the builder runs.

**Thread-local destruction (bug found and fixed).** A `thread_local` object
destroyed after the pool's thread-exit hook, for example a `thread_local`
`std::pmr::vector` constructed before the thread's first allocation, freed
its own blocks after the hook had retired the heap. That was reported as a
contract violation and aborted the process, although no other thread was
involved.

The exit hook now keeps the thread id and marks the thread as exiting.
Later calls from that thread take locked slow paths:
- frees of its own blocks go to its retired heap, and spans that become
  empty return to the pool;
- allocations use that heap as well.

Frees by other threads after the owner began to exit are still detected and
terminate the process (`misuse.free_after_owner_exit`). Tests:
`thread_local_destructors_may_free_the_threads_own_blocks`,
`thread_local_destructors_may_allocate_after_exit_processing`.

## 3. Capacity and reservation accounting

New or redefined `PoolStats` fields:

| Field | Meaning |
| --- | --- |
| `capacity_bytes` | the budget |
| `reserved_bytes` | budget charged: arena carved for thread heaps and spans, plus oversized system memory; never above the budget |
| `system_reserved_bytes` (new) | memory obtained from the system: the whole arena, the pool object, live oversized allocations |
| `metadata_bytes` (redefined) | heap objects and the headers and fences of thread-owned spans (no longer the pool object) |
| `thread_free_bytes` (new) | free blocks inside spans owned by thread heaps, usable only by their owner |
| `free_span_count`, `free_span_bytes` (new) | free spans returned by exited threads, available to any thread |
| `active_thread_heap_count` (new) | heaps owned by running threads |
| `oversized_reserved_bytes` (new) | system bytes of live oversized blocks |
| `free_bytes` | unchanged (`capacity − live consumed`), now documented as a utilization figure, not an amount the caller can allocate |

Identities documented in `pool.hpp` and checked by the tests (exact when no
other thread uses the pool):

```
reserved_bytes   == metadata_bytes + live_consumed_bytes + thread_free_bytes + free_span_bytes
reserved_bytes   <= capacity_bytes
allocation_count == deallocation_count + live_allocations - pending_remote_frees
```

Oversized allocations were already charged against the budget; that is now
stated precisely. The documentation no longer claims the budget bounds the
memory obtained from the operating system. The arena is allocated in full
at construction and oversized blocks come on top of it, so system memory
can approach twice the budget. Carved spans stay charged for the pool's
lifetime.

Tests:
- `budget_covers_heaps_spans_and_oversized_blocks`: a mixed workload up to
  exhaustion, with the identities checked throughout.
- `oversized_blocks_return_their_budget_when_released`: reservation,
  system memory and cleanup; oversized blocks use only the unreserved
  budget.
- `carved_spans_stay_charged_against_the_budget`: span retention while the
  thread runs.
- `statistics_identities_hold_for_every_size_and_alignment`.
- Free-span checks in the span-reuse thread test.
- An engine test pinning the exact bookkeeping sizes.

## 4. Benchmark method

- **Single thread:**
  - the checksum is computed from the trace and validated for every replay;
  - allocator order rotates (*i* mod *n*);
  - 2 warmups;
  - medians with MAD and min-max ranges;
  - the harness builds unchanged against earlier library versions.
- **Multithreaded:**
  - a timed drain phase;
  - `free` (enqueue) versus `done` (completed, including the drain);
  - per-thread checksums validated every round;
  - rotating allocator order with repeats;
  - only thread-safe allocators compared (no shared, unsynchronized buddy
    pool).
- **`nanoalloc_paired`:** two library versions in one process, alternating
  every iteration. A self-comparison showed that a single process can be
  biased by up to 20% through memory and code placement, so results come
  from several processes, with the self-comparison as the noise floor.
- **`compare_builds.py`:** separate-process comparisons in rotating order.
- **CI:** a manual run with `compare_with` runs the paired comparison on
  Linux and Windows (three processes per reference).

Hosted runners change CPU model between runs (Intel Ice Lake, AMD Zen 3,
AMD Zen 4 were seen). Only results from the same run are compared.

## 5. Performance

Profiling (callgrind) showed the regressions were on the allocation side;
frees already executed fewer instructions than the buddy allocator. Three
causes:
- The general split path did redundant work on every call.
- `Pool::allocate` saved and restored registers on every call.
- In `medium`/`long_short`, data-dependent list-empty checks were
  mispredicted.

The kept changes (R1, R3, R5, R5b in PERFORMANCE.md):
- a lean split path for exact-range misses;
- a frame-free throwing fast path, with failures thrown from the
  out-of-line paths;
- branch-free list updates only where they are mispredicted.

Allocation instructions per operation fell from 102-168 to 87-133, and
`long_short` mispredictions from 1.21 to 0.91 per allocation.

Time ratio current / `05f2ee7`, full-scale traces, median over processes
(below 1 is faster):

| Workload | local Intel, GCC (5 processes) | Zen 3, GCC (3) | Zen 3, MSVC (3) |
| --- | ---: | ---: | ---: |
| small | 0.98 | 0.98 | 1.00 |
| medium | 0.96 | 0.91 | 0.93 |
| mixed | 0.95 | 0.96 | 0.96 |
| lifo | 0.85 | 0.91 | 0.92 |
| random | 0.92 | 0.92 | 0.94 |
| bursts | 0.96 | 0.95 | 1.04 |
| long_short | 0.91 | 0.91 | 0.93 |
| fragmentation | 0.94 | 0.96 | 0.99 |
| reuse | 0.97 | 0.96 | 0.98 |

The self-comparison noise floor is within 0.99-1.01 for most workloads
(`lifo` 0.96).

The improvement is not universal:
- `bursts` with MSVC on Zen 3 is 4% slower.
- Against the buddy allocator, `long_short` (1.03-1.11), `reuse`
  (1.01-1.20) and, on Zen 3, `medium` (1.14-1.15) remain slower, depending
  on the CPU.

Rejected experiments, all measured:
- branch-free list updates everywhere (+10% on `small`);
- prefetching the next list node (no effect);
- relinking next-merged blocks in place (no effect on `reuse`).

## 6. Validation

Local (Linux, GCC 13.3 and Clang 18, warnings as errors):

| Configuration | Result |
| --- | --- |
| GCC Debug, GCC Release | 20/20 tests |
| Clang Debug, Clang Release | 20/20 tests |
| GCC ASan + UBSan, Clang ASan + UBSan | 22/22 tests (incl. ASan use-after-free and overflow detection) |
| Clang TSan | 19/19 tests; thread tests repeated 5/5, pmr and Allocator tests 3/3 |
| GCC shared library + install, `find_package` consumer (shared and static) | 20/20 tests; consumer builds and runs |
| Export without `.git` (`git archive`), Release | 20/20 tests |

CI (GitHub-hosted runners) builds and tests the same configurations on Linux
(GCC, Clang, sanitizers, TSan, shared library with install) and Windows
(MSVC Debug, Release, Debug + ASan). All jobs passed on the final commits.

Found and fixed during validation:
- **Package config.** The installed CMake package did not call
  `find_dependency(Threads)` although `nanoalloc::nanoalloc` links
  `Threads::Threads` publicly, so `find_package(nanoalloc)` failed in
  consuming projects. CI now builds and runs a consumer
  (`tests/package`) against the install tree.
- **MSVC.** C4702 (unreachable code after the throwing branch) under
  warnings as errors.
- **MSVC test assumption.** The cross-thread `Allocator` test assumed
  `std::map` allocates only its nodes; MSVC's also allocates a sentinel
  node.

## 7. Remaining limitations

- Allocation in workloads with many distinct short-lived sizes
  (`long_short`, `medium`) misses the exact bucket about half the time and
  splits; on some CPUs the buddy allocator is faster there.
- `reuse`-like patterns free into exact buckets whose size changes on every
  merge, so nearly every free relinks blocks.
- Peaks are sums of per-heap peaks (an upper bound with several threads).
- Spans stay with their thread while it runs, and carved spans stay charged
  to the budget for the pool's lifetime.
- Blocks a thread leaves allocated at exit stay allocated, with their
  spans, until the pool is destroyed.
- Multithreaded before/after differences could not be resolved on the
  available machines (bimodal run-to-run speed); none is claimed.
