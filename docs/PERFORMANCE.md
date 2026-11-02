# nanoalloc performance log

This log records how the Fast & Furious engine (16-byte granular blocks,
thread-local spans, bitmap-indexed buckets, inbox-based remote frees) is
measured, how it was optimized, and how it compares with the original buddy
allocator (`main`, tag `v0.1.0-buddy-baseline`) and the system allocator.

The second part of the log ("Review follow-up") covers the investigation of
the `long_short`, `fragmentation` and `reuse` regressions reported against
commit `05f2ee7`, and the measurement method that came out of it.

## Environments

| Name | CPU | OS, compiler | Notes |
| --- | --- | --- | --- |
| local | Intel Xeon @ 2.80 GHz (Cascade Lake), 4 vCPUs | Linux 6.18, GCC 13.3, glibc 2.39 | Shared KVM guest; identical runs differ by up to 2x in throughput |
| ci-zen3 | AMD EPYC 7763 (Zen 3), 4 vCPUs | GitHub-hosted ubuntu-24.04 (GCC 13.3) and windows-2022 (MSVC 19.44) | Paired benchmark job of CI run 41507316548 |
| ci-other | Intel Xeon Platinum 8370C; AMD EPYC 9V74 (Zen 4) | windows-2022 (MSVC 19.44) | Earlier paired runs; hosted runners change CPU model between runs |

All builds are CMake Release (`-O3 -DNDEBUG` or MSVC `/O2`), static library.
Profiling uses valgrind/callgrind 3.22 (instruction counts, branch and cache
simulation); `perf` is not available on these machines.

## Method

### Single-thread benchmark (`nanoalloc_bench`)

- Nine fixed workloads (unchanged since the baseline), each a trace
  generated from a fixed seed before timing. Every allocator replays the same
  trace through the same code; only allocate/deallocate differ.
- Each allocation stamps its first 8 bytes and last byte; each deallocation
  reads them. The expected checksum is computed from the trace, and every
  replay, warmup included, must reproduce it, or the run stops with an error.
- 2 warmup and 9 measured iterations per (workload, allocator). The
  allocator order rotates (iteration *i* starts with allocator *i* mod *n*).
- Medians are reported with their spread: median absolute deviation
  ("noise") and the min-max range of combined throughput.
- Memory is measured in a separate, untimed replay of the same trace.
- The arena size is derived from the trace (at least twice its peak live
  bytes) and is the same for every nanoalloc variant.

### Paired comparison of two versions (`nanoalloc_paired`)

Run-to-run drift on shared machines (2x between identical runs here) hides
differences of 5-20%. `nanoalloc_paired` links the current sources and a
second source tree (compiled with its namespace renamed) into one program.
It alternates the two versions iteration by iteration on the same traces,
with the same arena sizing and checksum validation, and reports the median
ratio *current time / reference time* with its interquartile range.

A self-comparison (the current tree against an identical copy, "A/A") showed
that one process can favour one side consistently, for example by 19% on
`reuse` allocation, through memory and code placement (address-space layout
randomization, physical pages). Results are therefore taken from several
processes. Each table below reports the median of the per-process medians
and the range across processes. The A/A run gives the noise floor.

### Multithreaded benchmark (`nanoalloc_mtbench`)

- `exchange`: each round, every thread allocates N objects, then all threads
  free them (a fixed share by another thread than the allocator), then every
  thread drains its inbox (`drain_pending_frees()`; nothing for the system
  allocator). Reported:
  - `alloc`: allocations.
  - `free`: deallocate() calls only, i.e. remote enqueue for remote frees.
  - `done`: free plus drain, after which every block is reusable.
  - `combined`: all operations over all three phases.
- `many-to-one`: thread 0 allocates, the other threads free into its inbox,
  thread 0 drains; `free` and `done` as above.
- Per-thread stamp sums are validated every round.
- Each configuration runs twice per allocator in rotating order. Medians are
  taken over all measured rounds, with the median absolute deviation.
- Only thread-safe allocators are compared (the system allocator and
  nanoalloc, each shared by all threads). The buddy baseline is not
  thread-safe and is compared single-threaded only.

## Reproducing

```bash
cmake --preset release
cmake --build --preset release
./build/release/benchmarks/nanoalloc_bench --allocators system,nanoalloc --csv bench.csv
./build/release/benchmarks/nanoalloc_mtbench

# Paired comparison with another version (any nanoalloc source tree):
git worktree add ../nanoalloc-ref 05f2ee7
cmake -S . -B build-paired -DCMAKE_BUILD_TYPE=Release \
      -DNANOALLOC_REFERENCE_SOURCE_DIR="$PWD/../nanoalloc-ref"
cmake --build build-paired --target nanoalloc_paired
for i in 1 2 3 4 5; do ./build-paired/benchmarks/nanoalloc_paired --iterations 15; done

# Separate-process comparison of nanoalloc_bench builds (rotating order):
python3 benchmarks/compare_builds.py --runs 6 a=path/to/bench_a b=path/to/bench_b -- --allocators nanoalloc
```

In CI, a manual run of the CI workflow with `compare_with` set (for example
`05f2ee7 main`) builds the paired comparison against each ref on Linux and
Windows and runs it in three processes.

## Single-thread results

Absolute numbers, local machine (median of 3 runs of `nanoalloc_bench`,
nanoalloc range across the runs):

| Workload | alloc ns system | alloc ns nanoalloc | free ns system | free ns nanoalloc | combined Mops/s system | combined Mops/s nanoalloc (range) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| small | 11.1 | 10.5 | 20.6 | 22.0 | 63.1 | 61.6 (56.9-64.5) |
| medium | 50.7 | 34.5 | 30.5 | 33.1 | 24.6 | 29.6 (29.1-32.6) |
| mixed | 37.5 | 22.0 | 27.8 | 26.0 | 30.6 | 41.7 (35.9-43.3) |
| lifo | 73.2 | 21.9 | 43.5 | 20.0 | 17.1 | 47.8 (41.0-53.0) |
| random | 41.5 | 20.6 | 32.6 | 26.4 | 27.0 | 42.5 (41.8-43.5) |
| bursts | 16.0 | 14.8 | 16.2 | 14.3 | 62.1 | 68.7 (65.5-71.1) |
| long_short | 13.6 | 23.0 | 16.9 | 25.7 | 65.5 | 41.1 (36.4-41.5) |
| fragmentation | 44.0 | 14.8 | 24.6 | 31.5 | 29.2 | 43.2 (42.2-46.0) |
| reuse | 10.7 | 12.4 | 10.3 | 26.2 | 95.1 | 52.3 (50.3-52.3) |

glibc's thread cache serves `long_short` and `reuse` (recycled same-size
chunks) without splitting or merging, which nanoalloc's immediate
coalescing cannot match.

### Versus the buddy baseline (paired, time ratio current / buddy)

Below 1 means the current engine is faster. Full-scale traces; median over
processes [range across processes].

| Workload | local (GCC, 5 processes) | ci-zen3 Linux (GCC, 3) | ci-zen3 Windows (MSVC, 3) |
| --- | --- | --- | --- |
| small | 0.87 [0.82-0.90] | 0.93 [0.92-0.94] | 0.94 [0.92-0.94] |
| medium | 0.90 [0.84-0.96] | 1.15 [1.06-1.22] | 1.14 [1.13-1.18] |
| mixed | 0.78 [0.71-0.81] | 0.92 [0.90-0.94] | 0.98 [0.96-1.00] |
| lifo | 0.38 [0.33-0.40] | 0.47 [0.45-0.55] | 0.53 [0.48-0.56] |
| random | 0.68 [0.66-0.71] | 0.87 [0.80-0.89] | 0.93 [0.87-0.96] |
| bursts | 0.50 [0.47-0.52] | 0.61 [0.61-0.66] | 0.72 [0.71-0.74] |
| long_short | 1.03 [1.02-1.07] | 1.03 [1.03-1.04] | 1.11 [1.09-1.13] |
| fragmentation | 0.92 [0.88-0.95] | 0.99 [0.97-1.05] | 1.06 [1.05-1.08] |
| reuse | 1.01 [0.96-1.05] | 1.11 [1.06-1.23] | 1.20 [1.20-1.21] |

The buddy allocator's relative speed depends on the CPU: on the Zen 3
runners it is faster than the current engine in `medium`, `long_short` and
`reuse` (and with MSVC in `fragmentation`), on the local Intel machine only
in `long_short`. Allocation is the slower side in `medium` and
`long_short` (current/buddy allocation time 1.31-1.55 on Zen 3), freeing in
`reuse` (1.12-1.18).

## Memory

From the untimed replay (MiB unless noted; identical before and after the
review changes, see below):

| Workload | peak requested | consumed buddy | consumed current | slack B/alloc buddy | slack B/alloc current | max slack (B) | reserved current |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small | 1.07 | 1.42 | 1.52 | 18.2 | 7.5 | 15 | 2.00 |
| medium | 4.78 | 6.34 | 4.87 | 396.8 | 7.5 | 15 | 6.00 |
| mixed | 7.21 | 9.49 | 7.39 | 261.0 | 7.5 | 15 | 9.00 |
| lifo | 12.07 | 16.10 | 13.20 | 83.5 | 7.5 | 15 | 14.00 |
| random | 5.15 | 6.88 | 5.43 | 144.7 | 7.5 | 15 | 6.00 |
| bursts | 0.18 | 0.24 | 0.23 | 29.5 | 7.5 | 15 | 1.00 |
| long_short | 17.92 | 23.92 | 18.48 | 55.4 | 7.5 | 15 | 19.00 |
| fragmentation | 3.68 | 4.90 | 4.19 | 43.8 | 7.5 | 15 | 5.00 |
| reuse | 0.01 | 0.02 | 0.02 | 16.0 | 7.5 | 15 | 1.00 |

"Consumed" includes the 16-byte headers; "reserved" is the budget charged
(thread heap and whole 1 MiB spans). Bookkeeping (`metadata_bytes`: heap
object and span headers) is 3.4-4.5 KiB per workload.

## Multithreaded results

Local machine, current engine; 20,000 objects of 16-511 bytes per thread
per round. 4 threads use all 4 vCPUs.

| threads | remote | allocator | alloc Mops/s | free Mops/s | done Mops/s | combined Mops/s | reserved MiB |
| ---: | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 0% | system | 20.6 | 25.0 | 25.0 | 22.0 | |
| 1 | 0% | nanoalloc | 61.5 | 25.6 | 25.6 | 36.4 | 4 |
| 2 | 0% | system | 42.5 | 49.7 | 49.7 | 45.7 | |
| 2 | 0% | nanoalloc | 139.5 | 52.1 | 52.1 | 73.9 | 8 |
| 2 | 50% | system | 28.7 | 7.9 | 7.9 | 12.3 | |
| 2 | 50% | nanoalloc | 73.0 | 12.3 | 10.9 | 18.5 | 8 |
| 2 | 100% | system | 15.3 | 9.2 | 9.2 | 11.4 | |
| 2 | 100% | nanoalloc | 86.8 | 42.1 | 21.1 | 34.0 | 8 |
| 4 | 0% | system | 75.2 | 75.0 | 75.0 | 76.2 | |
| 4 | 0% | nanoalloc | 242.9 | 101.4 | 101.4 | 140.6 | 16 |
| 4 | 25% | system | 40.0 | 11.9 | 11.9 | 18.4 | |
| 4 | 25% | nanoalloc | 134.5 | 27.2 | 26.7 | 44.6 | 16 |
| 4 | 100% | system | 28.2 | 6.7 | 6.7 | 11.0 | |
| 4 | 100% | nanoalloc | 101.8 | 19.1 | 14.0 | 24.7 | 16 |

| many-to-one threads | allocator | owner alloc Mops/s | free Mops/s | drain Mblocks/s | done Mops/s |
| ---: | --- | ---: | ---: | ---: | ---: |
| 2 | system | 19.5 | 24.1 | | 24.1 |
| 2 | nanoalloc | 52.7 | 39.1 | 59.0 | 23.4 |
| 4 | system | 19.1 | 7.9 | | 7.9 |
| 4 | nanoalloc | 44.4 | 15.3 | 62.6 | 12.0 |

- Remote frees are cheap to issue (`free`) but complete only when the owner
  drains (`done`). With 100% remote frees, draining takes about as long as
  issuing them, so `done` is roughly half of `free`.
- Reserved memory grows with the thread count (4 MiB per thread here):
  every thread keeps its spans while it runs.
- Repeated runs on the local machine fall into two speed modes (for example
  40 or 62-74 Mops/s for 1-thread allocation, for any build), so
  separate-process multithreaded runs cannot resolve before/after
  differences of the size measured single-threaded. No multithreaded
  before/after claim is made.

## Review follow-up (regressions in long_short, fragmentation, reuse)

### What was measured

The review reported these throughput ratios (current / buddy, Windows):
small 1.22, medium 0.93, mixed 1.18, LIFO 1.80, random 1.15, bursts 1.36,
long_short 0.75, fragmentation 0.71, reuse 0.75.

Separate-process runs on the local machine reproduced the pattern only
partly and inconsistently: identical runs differ by up to 2x, and the buddy
allocator is markedly faster on short traces (`--scale 0.1`) than on full
ones. The paired, multi-process method above was built to get stable
numbers.

### Profile

Per operation, timed replay at scale 0.1 (callgrind, deterministic):

| Workload | allocate instructions, buddy | allocate instructions, before | allocate mispredicts, buddy | allocate mispredicts, before | free instructions, buddy | free instructions, before |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| medium | 89 | 134 | 0.15 | 1.04 | 220 | 138 |
| long_short | 91 | 129 | 0.47 | 1.21 | 169 | 135 |
| fragmentation | 113 | 143 | | | 181 | 139 |
| reuse | 126 | 168 | | | 190 | 151 |

Path statistics (instrumented build, full traces):

| Workload | exact-bucket hits | splits | remainder moved to another bucket | 16-byte fragment created | split in place |
| --- | ---: | ---: | ---: | ---: | ---: |
| small | 90% | 10% | 2.5% | 5.1% | 2.0% |
| medium | 51% | 49% | 34% | 14% | 0.9% |
| long_short | 57% | 43% | 26% | 14% | 4.0% |
| fragmentation | 31% | 69% | 1.6% | 0.1% | 68% |
| reuse | 0% | 100% | 0% | 0% | 100% |

Findings:

- Frees already executed fewer instructions than the buddy allocator. The
  gap was on the allocation side.
- In `reuse` and `fragmentation`, almost every allocation goes through the
  general split path, carving a block from one large free block. That path
  paid for its generality on every call:
  - a large register save/restore;
  - re-checking the inbox and the exact bucket that the fast path had just
    checked, and recomputing the bucket;
  - the zero-remainder case and a generic list unlink, although the chosen
    block is always a list head that is larger than needed.
- `Pool::allocate` (the throwing API) also saved and restored registers on
  every call, exact hits included, only to test the engine's result for
  nullptr afterwards.
- In `medium` and `long_short`, half of the allocations split a slightly
  larger block and move the remainder to another bucket. The hit/miss
  decision and "is the list (or does it run) empty" are data-dependent: 1.2
  mispredictions per allocation in `long_short` against 0.5 for the buddy
  allocator, which uses 6 power-of-two classes where nanoalloc uses exact
  16-byte classes.

### Changes

Measured with `nanoalloc_paired` (time ratio new / previous step; local
machine, at least 150 alternating iterations in one process each, before
the per-process bias described under Method was known; the multi-process
before/after tables below confirm the combined effect):

| Step | Change | Effect | Decision |
| --- | --- | --- | --- |
| R1 | `allocate_larger()`: a dedicated split path for requests up to 4 KiB whose exact bucket is empty. Same block choice (best fit) as before, without the redundant checks and general cases | allocate instructions -7% to -14%; allocation time -2.5% to -9% on all workloads; frees unchanged | kept |
| R2 | Branch-free list updates everywhere (bitmap bits, back links through a scratch node, peaks via max) | long_short -6%, reuse -4%, but small +10% and fragmentation +5% (well-predicted branches replaced by extra work) | rejected |
| R3 | Failure policy as a compile-time parameter: the out-of-line paths throw `std::bad_alloc` themselves, so the inline fast path of `Pool::allocate` needs no stack frame | allocation time -3% to -8% on all workloads | kept |
| R4 | Prefetch the next list node when popping a head | within noise | rejected |
| R5 | Branch-free list updates only in `allocate_larger()`'s remainder move, where they are mispredicted | long_short allocation -8.7% (full scale), mixed -6%, others within noise | kept |
| R5b | The scratch node of R5 moved into existing padding (it had grown the heap object by 64 bytes and shifted every span) | equal to R5 on Zen 4 (MSVC); keeps the arena layout unchanged | kept |
| R6 | Free path: when a block merges only with the following free block and stays in its bucket, take over its list position instead of unlink + push | bursts frees -11%, reuse unchanged (its merged blocks stay in exact buckets, which change on every merge) | rejected (no effect on the target workloads) |

Instructions and mispredictions per allocation after R1-R5b (callgrind,
scale 0.1):

| Workload | allocate instructions before | after | allocate mispredicts before | after |
| --- | ---: | ---: | ---: | ---: |
| small | 102 | 87 | | 0.11 |
| medium | 134 | 117 | 1.04 | 0.84 |
| long_short | 129 | 112 | 1.21 | 0.91 |
| fragmentation | 143 | 116 | | 0.02 |
| reuse | 168 | 133 | | 0.01 |

The changes alter neither the block chosen for any request nor the memory
layout. The memory replay (peaks, slack, splits, merges) is identical
before and after on all nine workloads; only `metadata_bytes` is 256 bytes
lower, because it no longer counts the pool object (see the accounting
changes).

### Before / after (paired, time ratio current / 05f2ee7)

Full-scale traces; median over processes [range]; below 1 is faster.

| Workload | A/A noise floor, local | local (GCC, 5 processes) | ci-zen3 Linux (GCC, 3) | ci-zen3 Windows (MSVC, 3) |
| --- | --- | --- | --- | --- |
| small | 1.00 [0.92-1.03] | 0.98 [0.96-1.16] | 0.98 [0.97-0.98] | 1.00 [1.00-1.00] |
| medium | 0.99 [0.99-1.00] | 0.96 [0.91-0.97] | 0.91 [0.91-0.92] | 0.93 [0.92-0.95] |
| mixed | 1.00 [1.00-1.03] | 0.95 [0.86-0.96] | 0.96 [0.94-0.97] | 0.96 [0.94-0.97] |
| lifo | 0.96 [0.87-1.10] | 0.85 [0.81-0.93] | 0.91 [0.90-0.94] | 0.92 [0.91-0.98] |
| random | 0.99 [0.97-0.99] | 0.92 [0.91-0.95] | 0.92 [0.91-0.95] | 0.94 [0.94-0.94] |
| bursts | 1.01 [0.97-1.02] | 0.96 [0.94-1.40] | 0.95 [0.94-0.95] | 1.04 [1.03-1.05] |
| long_short | 0.99 [0.98-1.01] | 0.91 [0.86-0.92] | 0.91 [0.84-0.92] | 0.93 [0.92-0.93] |
| fragmentation | 0.99 [0.96-1.01] | 0.94 [0.93-0.98] | 0.96 [0.93-0.96] | 0.99 [0.98-0.99] |
| reuse | 1.01 [1.00-1.02] | 0.97 [0.93-1.25] | 0.96 [0.94-0.96] | 0.98 [0.97-1.07] |

Allocation alone (same runs): local 0.82-0.94, ci-zen3 Linux 0.83-0.92,
ci-zen3 Windows 0.83-0.98 except `bursts` 1.07. Deallocation is unchanged
within noise (0.97-1.03; locally `lifo`, the noisiest workload, 0.88).

Not universal: `bursts` with MSVC on Zen 3 is 4% slower (allocation +7%),
and `small` and `fragmentation` with MSVC are within noise. The gaps to the
buddy allocator listed above remain on Zen 3.

## Optimization log (initial implementation)

Instruction counts are per allocate+free pair in the timed replay at scale
0.05 (callgrind). "v1" is the first correct implementation (commit
`4773032`).

| Step | Change | Instructions/pair (small, medium, reuse, long_short, fragmentation, lifo) | Timed effect | Decision |
| --- | --- | --- | --- | --- |
| v1 | First correct implementation | 331, 346, 370, 339, 352, 365 | - | baseline |
| 1 | Exact-bucket head check before the bitmap search; bucket bound tables; 16-byte alignment fast path | 331, 353, 374, 346, 354, 361 | within noise | kept as a simplification |
| 2 | Lean counters (live and allocation counts derived); inlinable exact-fit fast path, split path out of line | 313, 341, 380, 330, 347, 367 | small alloc -17%, mixed alloc -7% | kept |
| 3 | All slow paths out of line and reached by tail calls | 291, 320, 359, 309, 325, 345 | vs v1: small alloc -30%, mixed alloc -14%, frees about -5% | kept |
| 4 | Serve small requests from large span blocks before smaller exact buckets | not measured | small alloc +36% and free +33% | reverted |
| 5 | Remove the cache-line alignment of the inbox and of the owner's bucket state | n/a | many-to-one remote frees 30-40% slower | alignment kept |

## Remaining gaps and ideas not pursued

- `long_short` and `medium` allocation (Zen 3: 1.3-1.55x the buddy
  allocator's time): exact 16-byte classes with immediate coalescing hit
  the exact bucket for only about half of the requests; the rest split and
  move a remainder. Power-of-two classes reuse freed blocks more often, at
  the cost of 55-400 bytes of slack per allocation.
- `reuse` frees (Zen 3: 1.1-1.2x): every merge in the exact-size range
  changes the bucket, so nearly every free unlinks a neighbour and pushes
  the merged block.
- Not tried: per-workload tuning of the bucket layout, or batching
  remote frees per owner (an API change).
