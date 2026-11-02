# nanoalloc v0.1 benchmark results

Raw output of `nanoalloc_bench` (default options: 1 warmup + 9 measured
iterations, scale 1, seed 0x6E616E6F) for the v0.1 buddy engine. See the
[README](../README.md#benchmarks) for the method and a summary.

Environment for both runs:

- Linux 6.18 x86_64, Ubuntu 24.04, glibc 2.39 (system allocator: glibc
  `malloc`/`free` with default tunables)
- Intel Xeon @ 2.80 GHz (family 6 model 85, Cascade Lake), 4 vCPUs in a
  shared KVM virtual machine; run-to-run noise is noticeable (see the `noise`
  column) and the two runs were taken minutes apart
- CMake Release build (`-O3 -DNDEBUG`), static library
- `nanoalloc` = default `PoolConfig`; `nanoalloc-tracked` =
  `track_requested_bytes = true`

## GCC 13.3.0

```
nanoalloc benchmark 0.1.0
  compiler   GCC 13.3.0
  build      Release, flags "-O3 -DNDEBUG"
  platform   Linux 6.18.44-fc-v185 x86_64
  cpu        Intel(R) Xeon(R) Processor @ 2.80GHz (4 logical CPUs)
  system     glibc 2.39 malloc/free
  method     1 warmup + 9 measured iterations, medians; scale 1; seed 0x6e616e6f

Workloads
  small          16-128 B objects, 20k live, random replacement in batches of 32-128
                 1000038 allocations, 24430 segments, peak 20000 live objects / 1.07 MiB requested, nanoalloc arena 4.00 MiB
  medium         128-4096 B objects, 4k live, random replacement in batches of 32-128
                 400027 allocations, 9832 segments, peak 4000 live objects / 4.78 MiB requested, nanoalloc arena 16.00 MiB
  mixed          80% 16-256 B, 18% 256 B-4 KiB, 2% 4-64 KiB; 8k live, random replacement
                 600008 allocations, 14832 segments, peak 8000 live objects / 7.21 MiB requested, nanoalloc arena 16.00 MiB
  lifo           50k objects of 16-1024 B allocated in sequence, freed in reverse order
                 1000000 allocations, 40 segments, peak 50000 live objects / 12.07 MiB requested, nanoalloc arena 32.00 MiB
  random         16-2048 B, random alloc/free batches of 16-256, random victims, ~10k live
                 600015 allocations, 4436 segments, peak 12489 live objects / 5.15 MiB requested, nanoalloc arena 16.00 MiB
  bursts         bursts of 64-1024 short-lived 16-256 B objects freed FIFO, 1k background
                 1000262 allocations, 3742 segments, peak 2023 live objects / 0.18 MiB requested, nanoalloc arena 1.00 MiB
  long_short     short-lived 16-512 B batches interleaved with up to 25k long-lived 16 B-4 KiB
                 800040 allocations, 24216 segments, peak 25192 live objects / 17.92 MiB requested, nanoalloc arena 64.00 MiB
  fragmentation  fill 20k x 32-64 B, free every other, add 2.5k x 512-2048 B, refill holes
                 812500 allocations, 100 segments, peak 22500 live objects / 3.68 MiB requested, nanoalloc arena 8.00 MiB
  reuse          batches of 256 objects of 40-56 B, allocated and freed repeatedly
                 1024000 allocations, 8000 segments, peak 256 live objects / 0.01 MiB requested, nanoalloc arena 1.00 MiB

Runtime (median of measured iterations)
workload       allocator              alloc       free   combined     alloc      free      total    noise
                                     Mops/s     Mops/s     Mops/s     ns/op     ns/op         ms        %
small          system                 81.80      48.60      60.98     12.22     20.57      32.77      4.0
               nanoalloc              86.48      32.04      46.76     11.56     31.21      42.61     14.6
               nanoalloc-tracked      71.89      31.24      43.56     13.91     32.01      45.91      9.9
medium         system                 19.06      31.58      23.77     52.46     31.66      33.60      1.6
               nanoalloc              27.71      22.97      25.12     36.09     43.54      31.84      4.4
               nanoalloc-tracked      27.61      22.08      24.54     36.22     45.29      32.71      1.1
mixed          system                 27.63      39.25      32.43     36.19     25.48      36.89      2.6
               nanoalloc              43.11      27.09      33.27     23.19     36.92      35.77      5.6
               nanoalloc-tracked      36.10      24.61      29.26     27.70     40.64      40.80      9.0
lifo           system                 13.07      21.96      16.39     76.51     45.53     121.90     11.4
               nanoalloc              17.96      13.19      15.21     55.69     75.83     131.52      5.7
               nanoalloc-tracked      16.77      11.86      13.89     59.62     84.32     143.49      5.2
random         system                 23.34      29.74      26.15     42.84     33.62      45.91     10.2
               nanoalloc              32.19      22.85      26.73     31.06     43.76      44.89      9.1
               nanoalloc-tracked      26.76      19.84      22.78     37.36     50.42      53.25     15.2
bursts         system                 44.00      45.48      44.73     22.73     21.99      44.73      6.2
               nanoalloc              44.30      24.86      31.85     22.57     40.23      62.82     14.1
               nanoalloc-tracked      38.13      24.42      29.77     26.22     40.95      67.19     14.5
long_short     system                 57.86      47.68      52.28     17.28     20.97      30.53     15.6
               nanoalloc              47.94      26.82      34.40     20.86     37.29      47.43     14.4
               nanoalloc-tracked      42.71      25.82      32.18     23.42     38.73      49.85     12.9
fragmentation  system                 21.41      38.94      27.63     46.71     25.68      58.31      5.5
               nanoalloc              58.58      30.78      40.36     17.07     32.48      40.55      8.1
               nanoalloc-tracked      54.09      25.89      35.02     18.49     38.62      46.22     10.5
reuse          system                102.03     114.57     107.94      9.80      8.73      18.97      6.1
               nanoalloc              79.35      42.89      55.68     12.60     23.32      36.71      1.6
               nanoalloc-tracked      76.27      40.92      53.26     13.11     24.44      38.51      2.6

Memory (untimed replay of the same trace)
workload       allocator          peak req  peak use  peak con    slack   slack      max     arena   arena  metadata   meta
                                       MiB       MiB       MiB  B/alloc       %  slack B       MiB  util %       KiB      %
small          system                 1.07      1.21       n/a      7.5    13.5       31         -       -         -      -
               nanoalloc              1.07      1.42      1.42     18.2    32.7       63         4    35.5        98   2.39
               nanoalloc-tracked      1.07      1.42      1.42     18.2    32.7       63         4    35.5       354   8.64
medium         system                 4.78      4.81       n/a      8.5     0.7       31         -       -         -      -
               nanoalloc              4.78      6.34      6.34    396.8    33.3     2047        16    39.6       386   2.36
               nanoalloc-tracked      4.78      6.34      6.34    396.8    33.3     2047        16    39.6      1410   8.61
mixed          system                 7.21      7.27       n/a      8.1     1.0       31         -       -         -      -
               nanoalloc              7.21      9.49      9.49    261.0    33.0    32767        16    59.3       386   2.36
               nanoalloc-tracked      7.21      9.49      9.49    261.0    33.0    32767        16    59.3      1410   8.61
lifo           system                12.07     12.43       n/a      7.5     3.0       31         -       -         -      -
               nanoalloc             12.07     16.10     16.10     83.5    33.2      511        32    50.3       770   2.35
               nanoalloc-tracked     12.07     16.10     16.10     83.5    33.2      511        32    50.3      2818   8.60
random         system                 5.15      5.24       n/a      8.1     1.9       31         -       -         -      -
               nanoalloc              5.15      6.88      6.88    144.7    33.2     1023        16    43.0       386   2.36
               nanoalloc-tracked      5.15      6.88      6.88    144.7    33.2     1023        16    43.0      1410   8.61
bursts         system                 0.18      0.19       n/a      7.6     8.5       31         -       -         -      -
               nanoalloc              0.18      0.24      0.24     29.5    33.0      127         1    23.8        26   2.53
               nanoalloc-tracked      0.18      0.24      0.24     29.5    33.0      127         1    23.8        90   8.78
long_short     system                17.92     18.10       n/a      7.6     4.6       31         -       -         -      -
               nanoalloc             17.92     23.92     23.92     55.4    33.2     2047        64    37.4      1538   2.35
               nanoalloc-tracked     17.92     23.92     23.92     55.4    33.2     2047        64    37.4      5634   8.60
fragmentation  system                 3.68      3.85       n/a      7.7     5.8       31         -       -         -      -
               nanoalloc              3.68      4.90      4.90     43.8    33.1     1023         8    61.2       194   2.37
               nanoalloc-tracked      3.68      4.90      4.90     43.8    33.1     1023         8    61.2       706   8.62
reuse          system                 0.01      0.01       n/a      7.1    14.7       29         -       -         -      -
               nanoalloc              0.01      0.02      0.02     16.0    33.3       24         1     1.6        26   2.53
               nanoalloc-tracked      0.01      0.02      0.02     16.0    33.3       24         1     1.6        90   8.78

Definitions
  alloc/free Mops/s  operations per second within allocation / deallocation segments
  combined Mops/s    all operations / (allocation time + deallocation time)
  total ms           wall time of one full trace replay, including timer reads
  noise              median absolute deviation of the total time across measured
                     iterations, relative to the median
  peak req           max over time of live requested bytes (from the trace)
  peak use           max over time of live usable bytes (usable_size / malloc_usable_size)
  peak con           max over time of arena bytes consumed by live allocations,
                     including per-allocation overhead (nanoalloc only: the C runtime
                     does not report the system allocator's per-chunk overhead exactly)
  slack              usable - requested, averaged over all allocations (B/alloc) and
                     relative to all requested bytes (%); max = worst single allocation
  arena / util       nanoalloc arena capacity / peak consumed as a share of it
  metadata           nanoalloc bookkeeping outside the arena (engine metadata plus the
                     requested-size table when tracking is enabled), and its share of
                     the arena
  nanoalloc          default PoolConfig (no requested-size tracking)
  nanoalloc-tracked  PoolConfig::track_requested_bytes = true

checksum 0x2eeb8345a96b36ec
```

## Clang 18.1.3

```
nanoalloc benchmark 0.1.0
  compiler   Clang 18.1.3 (1ubuntu1)
  build      Release, flags "-O3 -DNDEBUG"
  platform   Linux 6.18.44-fc-v185 x86_64
  cpu        Intel(R) Xeon(R) Processor @ 2.80GHz (4 logical CPUs)
  system     glibc 2.39 malloc/free
  method     1 warmup + 9 measured iterations, medians; scale 1; seed 0x6e616e6f

Workloads
  small          16-128 B objects, 20k live, random replacement in batches of 32-128
                 1000038 allocations, 24430 segments, peak 20000 live objects / 1.07 MiB requested, nanoalloc arena 4.00 MiB
  medium         128-4096 B objects, 4k live, random replacement in batches of 32-128
                 400027 allocations, 9832 segments, peak 4000 live objects / 4.78 MiB requested, nanoalloc arena 16.00 MiB
  mixed          80% 16-256 B, 18% 256 B-4 KiB, 2% 4-64 KiB; 8k live, random replacement
                 600008 allocations, 14832 segments, peak 8000 live objects / 7.21 MiB requested, nanoalloc arena 16.00 MiB
  lifo           50k objects of 16-1024 B allocated in sequence, freed in reverse order
                 1000000 allocations, 40 segments, peak 50000 live objects / 12.07 MiB requested, nanoalloc arena 32.00 MiB
  random         16-2048 B, random alloc/free batches of 16-256, random victims, ~10k live
                 600015 allocations, 4436 segments, peak 12489 live objects / 5.15 MiB requested, nanoalloc arena 16.00 MiB
  bursts         bursts of 64-1024 short-lived 16-256 B objects freed FIFO, 1k background
                 1000262 allocations, 3742 segments, peak 2023 live objects / 0.18 MiB requested, nanoalloc arena 1.00 MiB
  long_short     short-lived 16-512 B batches interleaved with up to 25k long-lived 16 B-4 KiB
                 800040 allocations, 24216 segments, peak 25192 live objects / 17.92 MiB requested, nanoalloc arena 64.00 MiB
  fragmentation  fill 20k x 32-64 B, free every other, add 2.5k x 512-2048 B, refill holes
                 812500 allocations, 100 segments, peak 22500 live objects / 3.68 MiB requested, nanoalloc arena 8.00 MiB
  reuse          batches of 256 objects of 40-56 B, allocated and freed repeatedly
                 1024000 allocations, 8000 segments, peak 256 live objects / 0.01 MiB requested, nanoalloc arena 1.00 MiB

Runtime (median of measured iterations)
workload       allocator              alloc       free   combined     alloc      free      total    noise
                                     Mops/s     Mops/s     Mops/s     ns/op     ns/op         ms        %
small          system                 71.00      40.53      51.61     14.08     24.67      38.76     12.6
               nanoalloc              73.19      29.40      41.95     13.66     34.02      48.15      8.0
               nanoalloc-tracked      57.56      27.45      37.17     17.37     36.43      53.30      8.2
medium         system                 17.96      29.87      22.44     55.67     33.48      35.66      4.6
               nanoalloc              23.65      22.75      23.19     42.28     43.96      34.14     11.5
               nanoalloc-tracked      17.49      18.52      17.99     57.17     54.01      45.49     19.9
mixed          system                 24.57      34.19      28.60     40.69     29.24      42.17     10.5
               nanoalloc              35.62      25.11      29.45     28.08     39.82      40.74      4.8
               nanoalloc-tracked      31.56      24.54      27.61     31.69     40.75      44.79      8.0
lifo           system                 14.45      25.23      18.38     69.19     39.63     107.26      9.4
               nanoalloc              15.96      12.61      14.09     62.66     79.28     141.93      6.2
               nanoalloc-tracked      15.86      12.19      13.78     63.05     82.05     145.09      3.9
random         system                 23.31      27.80      25.36     42.90     35.98      47.66     14.9
               nanoalloc              27.28      20.71      23.54     36.66     48.29      49.04     15.9
               nanoalloc-tracked      25.36      21.26      23.13     39.43     47.03      52.67      3.6
bursts         system                 60.78      64.08      62.39     16.45     15.61      32.07      3.8
               nanoalloc              47.49      28.98      36.00     21.06     34.50      55.58      1.0
               nanoalloc-tracked      38.72      26.71      31.61     25.83     37.43      64.24      3.8
long_short     system                 78.25      60.47      68.22     12.78     16.54      23.45      3.5
               nanoalloc              47.58      30.63      37.27     21.02     32.65      42.79      2.5
               nanoalloc-tracked      43.67      30.78      36.11     22.90     32.49      44.32      2.0
fragmentation  system                 22.01      44.65      29.48     45.44     22.40      55.12      9.3
               nanoalloc              48.65      30.55      37.53     20.55     32.74      43.30      6.1
               nanoalloc-tracked      43.68      25.25      32.00     22.89     39.60      50.78     14.1
reuse          system                109.14     125.38     116.69      9.16      7.98      17.72      1.4
               nanoalloc              61.35      41.36      49.41     16.30     24.18      41.45      2.7
               nanoalloc-tracked      58.94      39.51      47.31     16.97     25.31      43.29      4.8

Memory (untimed replay of the same trace)
workload       allocator          peak req  peak use  peak con    slack   slack      max     arena   arena  metadata   meta
                                       MiB       MiB       MiB  B/alloc       %  slack B       MiB  util %       KiB      %
small          system                 1.07      1.21       n/a      7.5    13.5       31         -       -         -      -
               nanoalloc              1.07      1.42      1.42     18.2    32.7       63         4    35.5        98   2.39
               nanoalloc-tracked      1.07      1.42      1.42     18.2    32.7       63         4    35.5       354   8.64
medium         system                 4.78      4.81       n/a      8.5     0.7       31         -       -         -      -
               nanoalloc              4.78      6.34      6.34    396.8    33.3     2047        16    39.6       386   2.36
               nanoalloc-tracked      4.78      6.34      6.34    396.8    33.3     2047        16    39.6      1410   8.61
mixed          system                 7.21      7.27       n/a      8.1     1.0       31         -       -         -      -
               nanoalloc              7.21      9.49      9.49    261.0    33.0    32767        16    59.3       386   2.36
               nanoalloc-tracked      7.21      9.49      9.49    261.0    33.0    32767        16    59.3      1410   8.61
lifo           system                12.07     12.43       n/a      7.5     3.0       31         -       -         -      -
               nanoalloc             12.07     16.10     16.10     83.5    33.2      511        32    50.3       770   2.35
               nanoalloc-tracked     12.07     16.10     16.10     83.5    33.2      511        32    50.3      2818   8.60
random         system                 5.15      5.24       n/a      8.1     1.9       31         -       -         -      -
               nanoalloc              5.15      6.88      6.88    144.7    33.2     1023        16    43.0       386   2.36
               nanoalloc-tracked      5.15      6.88      6.88    144.7    33.2     1023        16    43.0      1410   8.61
bursts         system                 0.18      0.19       n/a      7.6     8.5       31         -       -         -      -
               nanoalloc              0.18      0.24      0.24     29.5    33.0      127         1    23.8        26   2.53
               nanoalloc-tracked      0.18      0.24      0.24     29.5    33.0      127         1    23.8        90   8.78
long_short     system                17.92     18.10       n/a      7.6     4.6       31         -       -         -      -
               nanoalloc             17.92     23.92     23.92     55.4    33.2     2047        64    37.4      1538   2.35
               nanoalloc-tracked     17.92     23.92     23.92     55.4    33.2     2047        64    37.4      5634   8.60
fragmentation  system                 3.68      3.85       n/a      7.7     5.9       31         -       -         -      -
               nanoalloc              3.68      4.90      4.90     43.8    33.1     1023         8    61.2       194   2.37
               nanoalloc-tracked      3.68      4.90      4.90     43.8    33.1     1023         8    61.2       706   8.62
reuse          system                 0.01      0.01       n/a      7.1    14.7       29         -       -         -      -
               nanoalloc              0.01      0.02      0.02     16.0    33.3       24         1     1.6        26   2.53
               nanoalloc-tracked      0.01      0.02      0.02     16.0    33.3       24         1     1.6        90   8.78

Definitions
  alloc/free Mops/s  operations per second within allocation / deallocation segments
  combined Mops/s    all operations / (allocation time + deallocation time)
  total ms           wall time of one full trace replay, including timer reads
  noise              median absolute deviation of the total time across measured
                     iterations, relative to the median
  peak req           max over time of live requested bytes (from the trace)
  peak use           max over time of live usable bytes (usable_size / malloc_usable_size)
  peak con           max over time of arena bytes consumed by live allocations,
                     including per-allocation overhead (nanoalloc only: the C runtime
                     does not report the system allocator's per-chunk overhead exactly)
  slack              usable - requested, averaged over all allocations (B/alloc) and
                     relative to all requested bytes (%); max = worst single allocation
  arena / util       nanoalloc arena capacity / peak consumed as a share of it
  metadata           nanoalloc bookkeeping outside the arena (engine metadata plus the
                     requested-size table when tracking is enabled), and its share of
                     the arena
  nanoalloc          default PoolConfig (no requested-size tracking)
  nanoalloc-tracked  PoolConfig::track_requested_bytes = true

checksum 0x2eeb8345a96b36ec
```
