# nanoalloc evaluation package

A standalone grading tool that measures a nanoalloc implementation's
single-thread performance as a percentage of the frozen buddy baseline, on the
same machine, after checking that the implementation is correct.

`evaluate.py` is the single entry point. It takes two source trees:

- `--baseline`: the frozen buddy baseline (commit
  `f3aad1b4b884ad91a6f47eff6168c557fd78f206`; verified by content, no Git
  needed).
- `--candidate`: the implementation under evaluation.

It builds both independently with the same toolchain and flags, runs a trusted
correctness gate and timing driver against their public API, and writes a
report.

Nothing in this directory depends on any particular candidate. The workloads
are the original nine of the baseline's benchmark. Every candidate is built,
checked and measured by the same code. The package needs neither network
access nor Git metadata at run time.

## Contents

| Path | Purpose |
| --- | --- |
| `evaluate.py` | Entry point: builds, gate, A/A calibration, measurement, scoring, reports (Python 3.9+, standard library only) |
| `driver/CMakeLists.txt` | Builds one implementation tree together with the drivers, with flags set by the evaluator |
| `driver/timing.cpp` | Timing and memory-footprint driver |
| `driver/conformance.cpp` | Correctness gate |
| `driver/workloads.cpp`, `driver/workloads.hpp` | The nine original workloads, byte-for-byte copies of the baseline's `benchmarks/workloads.*` |
| `driver/common.hpp` | Output helpers |
| `baseline_manifest.json` | SHA-256 of every `include/` and `src/` file of the frozen baseline |
| `Dockerfile` | The evaluation environment |
| `selftest/make_slow_candidate.py` | Creates a deliberately slower valid copy of a tree, to check the performance gate |

## Requirements on a candidate tree

The candidate uses the baseline's layout and public API:

- `include/nanoalloc/pool.hpp`, `memory_resource.hpp` and `allocator.hpp`
  provide `nanoalloc::Pool`, `PoolConfig`, `PoolStats`, `MemoryResource` and
  `Allocator<T>` with the baseline's signatures and documented contract.
- Every `.cpp` file under `src/` belongs to the implementation and is compiled.
  Generated sources are not supported. `src/` is on the include path.
- Optional: a top-level `CMakeLists.txt` with tests. The evaluator builds the
  candidate with it and runs `ctest` as part of the correctness gate.

File names and internal structure are free; nothing has to be renamed.

## What it does

1. **Baseline check.** The `include/` and `src/` files of `--baseline` must
   match `baseline_manifest.json` exactly (override:
   `--allow-unverified-baseline`, reported).
2. **Builds.** The tool builds the baseline, an identical copy of it, and the
   candidate, each in its own directory and binary. Each build compiles the
   implementation's `src/**/*.cpp` and the drivers with the same compiler,
   `CMAKE_BUILD_TYPE` (Release by default), C++20 and extra flags. The
   implementation's own CMake files are not used for these builds. The report
   states whether the recorded compile flags are identical for all three.
3. **Correctness gate** (candidate):
   - Its own CMake build and `ctest` run (`--candidate-tests auto|on|off`;
     `auto` runs them when there are any).
   - The conformance driver, by category:

     | Category | Checks |
     | --- | --- |
     | api | the baseline's documented contract: capacity 0 rejected and capacity rounding; zero-size requests unique, non-null and releasable; alignments 1-4096 honoured; invalid alignment gives `std::invalid_argument` (`try_allocate`: nullptr); alignment above 4096 and requests above the capacity give `std::bad_alloc`; `owns()`; allocation, deallocation and live counts; free capacity fully restored after releasing everything; `std::pmr` and `Allocator<T>` with standard containers |
     | integrity | 200,000 random allocations and releases (sizes up to 266 KB, some aligned). Every byte of every reported `usable_size()` is written and checked, live `[ptr, ptr + usable_size)` ranges may not overlap, and `verify_integrity()` must hold. |
     | reuse | filling a 4 MiB pool to exhaustion with one size, releasing everything and filling it again must fit at least as many blocks |
     | budget | the payload handed out may not exceed the pool capacity |
     | packing | for every positive size from 1 to 4096 bytes, sizes up to 64 KiB and large sizes up to 5 MB, with and without alignment: `usable_size()` equals the size rounded up to 16 (at most 15 bytes of slack); `--packing report` makes this informational |

   - **Actual consumption.** Arena bytes per allocation, measured as pool
     capacity divided by the number of allocations of one size that fit in a
     4 MiB pool. This shows the real cost of each block whatever
     `usable_size()` reports, and over-reported usable sizes are caught by the
     integrity check, which writes every reported byte.
   - The conformance driver also runs on the baseline, for reference (the
     buddy baseline does not meet the packing requirement).
4. **A/A calibration.** The baseline is measured against its identical copy,
   exactly like a candidate.
5. **Measurement.** Baseline against candidate.
   - Each round runs every workload in one fresh process per implementation,
     the two processes back to back, the order alternating from pair to pair.
   - Each process generates the trace, chooses its capacity, builds the pool
     (all untimed), then replays the workload `--warmup` times unmeasured and
     `--iterations` times measured.
   - A measured replay is timed as a whole: every allocation and deallocation
     of the trace, plus a stamp written into each new block and read back
     before it is freed.
   - Every replay must reproduce the checksum computed from the trace,
     otherwise the run fails.
   - Each implementation gets the capacity it needs: twice the peak live
     requested bytes rounded up to a power of two, at least 1 MiB, doubled
     until an untimed replay succeeds. The capacities are reported.
6. **Memory footprint.** The smallest capacity with which each implementation
   completes each workload, found by bisection (untimed).
7. **Scores, verdict, reports.**

Defaults: 11 rounds, 2 warmup and 9 measured replays per process, scale 1.0
(the original operation counts). The minimums are 3 rounds, 1 warmup and 7
measured replays. Other scales are flagged as non-standard in the report.

## Scoring

For each workload, the median time of an implementation is the median over its
processes of each process's median replay time. Then:

```
speed_percent = 100 * buddy_median_time / candidate_median_time
overall       = geometric mean of the nine speed_percent values
```

100% is equal throughput; above 100% the candidate is faster.

**Uncertainty.** Each round's paired processes give one overall score (the
geometric mean of the nine per-workload ratios of that round). The interval is
a distribution-free confidence interval for the median round score, taken from
the sorted round scores: from the *k*-th lowest to the *k*-th highest, with
the largest *k* whose confidence 1 - 2 * P(Binomial(n, 1/2) <= k - 1) is at
least 98%. With the default 11 rounds that is the 2nd lowest to the 2nd
highest (98.8%), so a single disturbed round does not decide the verdict;
with 7 rounds it is the full range (98.4%). The interval is extended to include
the overall score if needed.

**Reliability.** The A/A calibration must score within `--max-aa-bias` of 100%
(default 3 points). The width of its interval must not exceed
`--max-aa-range` (default 30 points), a guard against pathological noise;
ordinary noise is handled by the decision interval. Otherwise the measurement
is not considered reliable.

**Verdict** with `--threshold T`:

| Verdict | Condition |
| --- | --- |
| FAIL | the correctness gate failed (build, own tests, conformance, a wrong checksum or failed allocation in any replay) |
| INCONCLUSIVE | the A/A calibration is outside its limits |
| PASS | the lower end of the decision interval is at least T |
| FAIL | the upper end of the decision interval is below T |
| INCONCLUSIVE | otherwise (T lies inside the decision interval) |

The decision interval is the measurement's interval widened by the A/A bias
on both sides. The threshold is only ever taken from `--threshold`.

Without `--threshold`, the verdict is MEASURED (or FAIL for a correctness
failure).

**Exit status:** 0 PASS or MEASURED, 1 FAIL, 2 INCONCLUSIVE, 3 evaluation
error (invalid inputs, the baseline does not verify or build, tools missing).

If only the packing requirement fails, the candidate is still measured, but
the verdict is FAIL unless `--packing report` is given.

## Output

Written to `--out` (default `./nanoeval-results`):

| File | Contents |
| --- | --- |
| `report.md` | Verdict; table of buddy time, candidate time, performance %, range of the round scores, spreads and A/A % per workload and overall; A/A calibration; correctness gate with every conformance check; actual consumption; memory footprint and capacities; environment |
| `results.json` | Everything in the report plus all raw per-process iteration times, self-reported statistics and build details |
| `results.csv` | One row per workload and comparison (calibration, measurement) |
| `evaluation.log`, `logs/` | Progress, build logs, driver and test output |
| `work/` | Staged trees and builds (can be deleted) |

The environment section records:
- CPU model, logical CPUs, affinity, cgroup CPU quota and governor;
- OS, kernel and memory;
- whether the run was in a container, and the image label;
- compiler, CMake and generator versions;
- build type and the exact implementation compile flags.

## Running

### Native (Linux)

Requirements: Python 3.9+, CMake 3.20+, a C++20 compiler (GCC 11+ or
Clang 14+), Ninja or Make.

```bash
python3 evaluation/evaluate.py \
    --baseline /path/to/nanoalloc-buddy-baseline \
    --candidate /path/to/candidate \
    --label candidate-name \
    --out results/candidate-name \
    --threshold 90
```

### Docker

Build the image once (network needed for the base image and packages only):

```bash
docker build -t nanoalloc-evaluation:1.0 evaluation/
```

Evaluate a candidate (no network):

```bash
docker run --rm --network none \
    -v /path/to/nanoalloc-buddy-baseline:/inputs/baseline:ro \
    -v /path/to/candidate:/inputs/candidate:ro \
    -v /path/to/results/candidate-name:/results \
    nanoalloc-evaluation:1.0 \
    --baseline /inputs/baseline --candidate /inputs/candidate \
    --label candidate-name --out /results --threshold 90
```

Add `--user "$(id -u):$(id -g)"` to get result files owned by the calling
user. `--pin-cpu N` runs the timing processes on one CPU.

## Packaging for an external grading environment

1. **Export the evaluator** (this directory only):
   ```bash
   tar czf nanoalloc-evaluation-1.0.tar.gz --exclude=__pycache__ \
       -C <repository> evaluation
   ```
2. **Export the frozen baseline** as a source-only snapshot, once, on a
   machine with the repository:
   ```bash
   git -C <repository> archive --prefix=nanoalloc-buddy-baseline/ \
       f3aad1b4b884ad91a6f47eff6168c557fd78f206 | gzip > nanoalloc-buddy-baseline.tar.gz
   ```
   The evaluator verifies the extracted snapshot against
   `baseline_manifest.json`, so it needs no Git afterwards. Keep the snapshot
   separate from the evaluator.
3. **Build the image** where network access is available, and export it for
   offline grading machines:
   ```bash
   tar xzf nanoalloc-evaluation-1.0.tar.gz
   docker build -t nanoalloc-evaluation:1.0 evaluation/
   docker save nanoalloc-evaluation:1.0 | gzip > nanoalloc-evaluation-1.0.image.tar.gz
   # on each grading machine:
   docker load < nanoalloc-evaluation-1.0.image.tar.gz
   ```
   If the default registry is rate-limited, add
   `--build-arg BASE_IMAGE=mirror.gcr.io/library/ubuntu:24.04` (the same
   image from a mirror).
4. **Grade.** Extract the baseline once, then run the `docker run` command
   above for each candidate source tree, with the same image, machine and
   settings for every candidate. Grade from each `report.md` / `results.json`
   and the exit status.

**Comparability.** Percentages are relative to the buddy baseline measured in
the same run, so they are comparable across candidates evaluated on the same
machine type. Absolute times are not. For a fair comparison, use one machine
(or identical machines) and an otherwise idle system. Never compare runs that
used different `--scale` or toolchains.

## Self-test

Checks of the evaluator itself, using only the baseline snapshot:

```bash
# A/A: the baseline against an identical copy scores about 100% (PASS at 90).
cp -r nanoalloc-buddy-baseline buddy-copy
python3 evaluation/evaluate.py --baseline nanoalloc-buddy-baseline --candidate buddy-copy \
    --packing report --threshold 90 --out results/aa

# A deliberately slower valid copy must be detected (FAIL at 90; the busy-wait
# per allocate/deallocate call costs roughly a third of the baseline's speed).
python3 evaluation/selftest/make_slow_candidate.py nanoalloc-buddy-baseline buddy-slow --spin 12
python3 evaluation/evaluate.py --baseline nanoalloc-buddy-baseline --candidate buddy-slow \
    --packing report --threshold 90 --out results/slow
```

`make_slow_candidate.py` works on any tree that defines `Pool::allocate` and
`Pool::deallocate` out of line in `src/`, so the same check can be applied to a
candidate. How much a given spin slows a tree depends on its speed and on the
machine.

## Limitations

- **Single-thread only.** The nine workloads are single-threaded, as is the
  baseline's `Pool`.
- **Noise.** Timing on shared or virtualized machines drifts. The paired,
  alternating processes and the A/A calibration measure this, and the verdict
  becomes INCONCLUSIVE rather than guessing. Rerun on a quieter machine, or
  with more rounds (`--processes`), if that happens.
- **Footprint search.** It assumes that a larger capacity never fails where a
  smaller one succeeds.
- **Actual consumption.** It counts arena bytes; bookkeeping an
  implementation keeps outside its arena is not included (the baseline's
  `PoolStats::metadata_bytes` is reported as self-reported data).
- **Build model.** The candidate's own build settings (in its CMake files) are
  used only for its own tests. Performance is measured with the evaluator's
  build of its sources.
