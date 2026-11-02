# nanoalloc performance log

This log records how the Fast & Furious engine (16-byte granular blocks,
thread-local spans, bitmap-indexed buckets, inbox-based remote frees) was
measured and optimized, and how it compares with the original buddy allocator
(tag `v0.1.0-buddy-baseline`, same benchmark sources) and the system
allocator.

## Environment

| | |
| --- | --- |
| CPU | Intel Xeon @ 2.80 GHz (family 6 model 85, Cascade Lake), 4 vCPUs |
| Machine | Shared KVM virtual machine; run-to-run noise of 2-15% on single-thread workloads and up to 2x on some multithreaded configurations |
| OS | Linux 6.18 x86_64, Ubuntu 24.04 |
| System allocator | glibc 2.39 `malloc`/`free`, default tunables |
| Compiler | GCC 13.3.0, CMake Release (`-O3 -DNDEBUG`), static library |
| Profiling | valgrind/callgrind 3.22 (instruction counts and cache simulation); `perf` is not available on this machine |

All numbers are medians of the measured iterations (single-thread: 1 warmup +
9 measured; multithreaded: 2 warmup + 9 measured rounds).

## Reproducing

```bash
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/nanoalloc_bench --allocators system,nanoalloc --csv ff.csv
./build/release/benchmarks/nanoalloc_mtbench
./build/release/benchmarks/nanoalloc_mtbench --threads 8,16   # oversubscribed on 4 vCPUs

# Baseline buddy allocator from the same benchmark sources:
git worktree add ../nanoalloc-baseline main
cmake -S ../nanoalloc-baseline -B build-baseline -DCMAKE_BUILD_TYPE=Release
cmake --build build-baseline
./build-baseline/benchmarks/nanoalloc_bench --allocators nanoalloc --csv buddy.csv
```

The workload generator and the benchmark method are unchanged from the
baseline, so the traces (sizes, order, seed) are identical for all
allocators. The only addition to the single-thread report is the
`reserved` column (memory the pool reserved by the end of the replay).

## Single-thread results

Per-operation times (ns) and combined throughput (allocations plus
deallocations per second):

| Workload | alloc ns system | alloc ns buddy | alloc ns new | free ns system | free ns buddy | free ns new | combined Mops/s system | buddy | new |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small | 10.8 | 11.0 | 10.5 | 18.9 | 29.8 | 20.3 | 67.4 | 49.0 | 64.8 |
| medium | 50.3 | 55.2 | 37.7 | 28.8 | 83.7 | 29.8 | 25.3 | 14.4 | 29.6 |
| mixed | 33.8 | 27.8 | 23.0 | 23.2 | 46.8 | 23.0 | 35.1 | 26.8 | 43.4 |
| lifo | 90.5 | 58.6 | 37.1 | 47.9 | 83.8 | 30.5 | 14.4 | 14.0 | 29.6 |
| random | 40.6 | 40.8 | 24.1 | 31.2 | 58.0 | 29.9 | 27.9 | 20.2 | 37.0 |
| bursts | 19.3 | 20.9 | 15.4 | 19.4 | 38.1 | 14.5 | 51.7 | 33.9 | 66.8 |
| long_short | 14.8 | 26.1 | 28.7 | 17.9 | 47.1 | 26.4 | 61.2 | 27.3 | 36.3 |
| fragmentation | 41.3 | 29.5 | 16.9 | 26.0 | 58.0 | 30.2 | 29.7 | 22.9 | 42.5 |
| reuse | 8.8 | 11.9 | 14.0 | 8.2 | 23.2 | 26.1 | 117.1 | 56.9 | 49.9 |

Memory (MiB unless noted; "usable" is `usable_size()`/`malloc_usable_size()`,
"consumed" adds per-allocation headers, "reserved" is spans plus thread
heaps):

| Workload | peak requested | usable system | usable buddy | usable new | slack B/alloc system | buddy | new | max slack new (B) | consumed buddy | consumed new | reserved new |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small | 1.07 | 1.21 | 1.42 | 1.21 | 7.5 | 18.2 | 7.5 | 15 | 1.42 | 1.52 | 2.00 |
| medium | 4.78 | 4.82 | 6.34 | 4.81 | 8.5 | 396.8 | 7.5 | 15 | 6.34 | 4.87 | 6.00 |
| mixed | 7.21 | 7.27 | 9.49 | 7.26 | 8.1 | 261.0 | 7.5 | 15 | 9.49 | 7.39 | 9.00 |
| lifo | 12.07 | 12.43 | 16.10 | 12.43 | 7.5 | 83.5 | 7.5 | 15 | 16.10 | 13.20 | 14.00 |
| random | 5.15 | 5.24 | 6.88 | 5.24 | 8.1 | 144.7 | 7.5 | 15 | 6.88 | 5.43 | 6.00 |
| bursts | 0.18 | 0.19 | 0.24 | 0.19 | 7.6 | 29.5 | 7.5 | 15 | 0.24 | 0.23 | 1.00 |
| long_short | 17.92 | 18.10 | 23.92 | 18.10 | 7.6 | 55.4 | 7.5 | 15 | 23.92 | 18.48 | 19.00 |
| fragmentation | 3.68 | 3.85 | 4.90 | 3.84 | 7.7 | 43.8 | 7.5 | 15 | 4.90 | 4.19 | 5.00 |
| reuse | 0.01 | 0.01 | 0.02 | 0.01 | 7.1 | 16.0 | 7.5 | 15 | 0.02 | 0.02 | 1.00 |

Observations:

- Payload slack is 7.5 bytes per allocation on average and never more than
  15 bytes, matching the system allocator's usable bytes; the buddy
  allocator wasted 16-397 bytes per allocation (about 33% of requested
  bytes).
- Consumed memory is below the buddy allocator's in every workload except
  `small` (1.52 vs 1.42 MiB), where the 16-byte header outweighs the rounding
  savings on 16-128-byte objects.
- Reserved memory is rounded up to whole 1 MiB spans; for one thread this
  adds at most about 1-2 MiB over the peak consumed bytes on these traces.
  glibc's per-chunk overhead is not measurable from the C runtime, so
  consumed memory is not compared with it.
- Higher combined throughput than the buddy allocator in 8 of 9 workloads.
  It is slower in `reuse` (all blocks coalesce back into the span's free
  block after every batch, so each allocation splits and each free merges),
  and `long_short` allocation alone is slower (28.7 vs 26.1 ns) although its
  frees are much faster.
- Against glibc: faster in `medium`, `mixed`, `lifo`, `random`, `bursts` and
  `fragmentation`, on par in `small`, slower in `long_short` and `reuse`,
  where glibc's thread cache serves recycled same-size chunks without any
  splitting or merging.

## Multithreaded results

`exchange`: each thread allocates 20,000 objects of 16-511 bytes per round,
then all threads free them; `remote` is the share freed by a thread other
than the allocator. Throughput is aggregate (operations / longest per-thread
phase time). 8 and 16 threads oversubscribe the 4 vCPUs.

| threads | remote | alloc Mops/s system | alloc Mops/s new | free Mops/s system | free Mops/s new | combined system | combined new | reserved new (MiB) |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0% | 12.9 | 66.9 | 14.4 | 25.1 | 13.6 | 35.6 | 4 |
| 2 | 0% | 33.7 | 80.8 | 46.1 | 44.0 | 36.6 | 62.1 | 8 |
| 2 | 10% | 28.7 | 57.7 | 10.5 | 19.2 | 14.7 | 29.3 | 8 |
| 2 | 25% | 27.6 | 79.9 | 7.4 | 20.5 | 11.5 | 32.6 | 8 |
| 2 | 50% | 18.4 | 58.0 | 4.1 | 16.7 | 6.3 | 25.7 | 8 |
| 2 | 100% | 28.8 | 28.7 | 19.5 | 43.9 | 21.8 | 34.8 | 8 |
| 4 | 0% | 45.6 | 129.1 | 48.7 | 50.8 | 47.1 | 73.5 | 16 |
| 4 | 10% | 33.1 | 108.0 | 8.4 | 27.1 | 13.5 | 44.3 | 16 |
| 4 | 25% | 33.2 | 102.5 | 6.6 | 18.0 | 11.0 | 29.7 | 16 |
| 4 | 50% | 31.3 | 120.8 | 5.2 | 23.7 | 8.8 | 39.1 | 16 |
| 4 | 100% | 23.8 | 32.6 | 4.6 | 29.9 | 7.7 | 27.4 | 16 |
| 8 | 0% | 33.1 | 148.1 | 31.5 | 94.1 | 33.0 | 113.2 | 32 |
| 8 | 25% | 26.8 | 128.3 | 8.0 | 24.8 | 12.1 | 43.4 | 32 |
| 8 | 100% | 42.8 | 63.2 | 7.4 | 22.4 | 10.6 | 31.9 | 32 |
| 16 | 0% | 28.2 | 301.6 | 30.1 | 47.0 | 27.7 | 80.6 | 64 |
| 16 | 25% | 37.0 | 206.8 | 10.8 | 15.2 | 16.9 | 28.3 | 64 |
| 16 | 100% | 28.7 | 29.8 | 8.3 | 20.1 | 12.9 | 24.6 | 64 |

`many-to-one`: thread 0 allocates 20,000 objects per other thread, the other
threads free them concurrently into thread 0's inbox, then thread 0 drains
explicitly with `drain_pending_frees()`.

| threads | owner alloc Mops/s system | owner alloc Mops/s new | remote free Mops/s system | remote free Mops/s new | drain Mblocks/s new |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2 | 12.3 | 68.6 | 17.5 | 27.0 | 64.0 |
| 4 | 12.4 | 37.5 | 6.1 | 8.8 | 49.1 |
| 8 | 18.8 | 22.3 | 8.9 | 8.3 | 29.9 |
| 16 | 16.0 | 21.6 | 6.2 | 12.7 | 33.5 |

Observations:

- Allocation scales with threads because each thread allocates from its own
  spans without locks or atomic read-modify-write operations.
- Cross-thread frees cost two CAS operations (block state, inbox head); with
  many threads freeing into the same owner the inbox head's cache line is
  contended, so many-to-one remote frees are roughly on par with glibc at 8
  threads. Draining is cheap (30-64 M blocks/s).
- Memory retention: every thread keeps the spans it acquired, so reserved
  memory grows with the thread count (4 MiB per thread here) even when the
  live data is the same. Spans are only returned to the pool when a thread
  exits.
- The 1-thread system figures are low because glibc returns the top of its
  heap to the OS after every round (default trim threshold) and faults it in
  again; that is the system allocator's default behaviour, not a harness
  artefact.

## Optimization log

Instruction counts are per allocate+free pair in the timed replay at scale
0.05 (callgrind; deterministic). Timings are means of two A/B runs with 15
iterations each, alternating binaries. "v1" is the first correct
implementation (commit `4773032`).

| Step | Change | Instructions/pair (small, medium, reuse, long_short, fragmentation, lifo) | Timed effect | Decision |
| --- | --- | --- | --- | --- |
| v1 | First correct implementation | 331, 346, 370, 339, 352, 365 | - | baseline |
| 1 | Exact-bucket head check before the bitmap search; bucket bound tables instead of recomputing bucket indices; 16-byte alignment fast path | 331, 353, 374, 346, 354, 361 | within noise | kept as a simplification |
| 2 | Lean counters (live count and allocation count derived, each counter read and written once); inlinable exact-fit fast path, split path out of line | 313, 341, 380, 330, 347, 367 | small alloc -17%, mixed alloc -7% | kept |
| 3 | All slow paths (drain, split, remote, diagnostics, TLS miss) out of line and reached by tail calls, so the hot paths need no saved registers | 291, 320, 359, 309, 325, 345 | vs v1: small alloc -30%, mixed alloc -14%, frees about -5% | kept |
| 4 | Serve small requests from large span blocks before smaller exact buckets ("large block first") | not measured | small alloc +36% and free +33%; long_short alloc -17%; reuse alloc -14% | reverted: large regression on small objects |
| 5 | Remove the cache-line alignment of the inbox and of the owner's bucket state | n/a | many-to-one remote frees 30-40% slower, exchange 100% remote slower | alignment kept (justified) |

Profiling notes:

- The engine executes about 290-360 instructions per allocate+free pair
  (including about 40 for the benchmark loop), against about 205 for glibc on
  the same trace. Data-cache misses were similar for both (callgrind cache
  simulation on `long_short`: 2.0 M vs 1.7 M L1 read misses), so the
  remaining gap on `long_short` and `reuse` is mainly work per operation:
  splitting and merging on every operation (immediate coalescing), list moves
  between exact buckets, and boundary-tag updates of neighbours.
- In `long_short` 43% of allocations miss the exact bucket and split a
  larger block; in `reuse` every allocation splits the span's free block
  because every batch coalesces back into it.

## Next potential improvements (not implemented)

- Reduce split/merge churn for alternating allocate/free of the same size
  without lazy coalescing, for example by checking whether the block just
  freed can satisfy the next request before merging its tail neighbour.
- Batch remote frees per owner inside one deallocation call when a caller
  frees many blocks of the same owner (an API such as `deallocate_batch`),
  so one inbox CAS covers several blocks.
- Return completely free spans of a live thread to the pool after a period
  of disuse, to bound per-thread retention (currently out of scope).
- Measure on a dedicated machine with `perf` to separate branch mispredictions
  and cache misses from instruction count.
