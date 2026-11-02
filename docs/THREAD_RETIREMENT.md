# Thread retirement and global reclamation

Allocations may outlive the thread that made them. A block stays valid until
it is deallocated or its pool is destroyed, and any thread may free it, before
or after the allocating thread has exited. This document describes how a
thread's heap is handed over when the thread exits, what synchronization
makes the hand-over safe, and what was measured.

The allocator itself is unchanged:
- 16-byte headers;
- exact 16-byte granularity;
- bitmap-indexed free lists with immediate coalescing;
- the lock-free per-owner inbox for cross-thread frees;
- the allocation and deallocation fast paths.

## Life of a heap

```
 active  --(thread exits, heap still holds blocks)-->  retired  --(last block released)-->  recyclable
   |                                                                                            |
   +------------------(thread exits, heap holds no block)---------------------------------------+
 recyclable  --(a new thread needs a heap)-->  active  (same object, statistics kept)
```

- **active**: owned by a running thread, the only thread that touches its
  free lists.
- **retired**: owned by the reclamation worker, the only thread that touches
  its free lists. Any thread may free its blocks through its inbox.
- **recyclable**: holds no span, hence no block and no pending free; nobody
  refers to it. The next new thread adopts it.

## The hand-over

The exit hook is a `thread_local` object armed by the thread's first heap. For
each pool it runs these steps:

1. Under the pool's lock, the owner releases the frees waiting in its inbox
   (the normal drain, with coalescing).
2. Still under the lock, it gives spans that are now completely free back to
   the pool's free-span list. These were its last operations on the heap.
3. It publishes the state: `retired` (release store), or `recyclable` if no
   span is left. The pool lock is released.
4. A retired heap is linked into the reclamation worker's list under the
   worker's mutex, and the worker is woken. If this is the first hand-over in
   the process, the worker thread is started.

When all pools are done, the hook clears the thread's cache of heap pointers
(`ThreadHeap*` per pool, in plain thread-local storage) and marks the thread
as exiting. That is what disables the thread's direct access to the heap; no
atomic operation is involved:

- The allocation and deallocation fast paths look up the cached heap. Finding
  none, they take the slow paths.
- The slow deallocation path compares the block's owner with the calling
  thread's heap (none), so every later free from this thread goes to the
  owner's inbox, exactly like a free from another thread.
- The slow allocation path sees the exiting mark and fails: `allocate()`
  throws `std::bad_alloc`, `try_allocate()` returns `nullptr`. This holds for
  every pool and for oversized requests. No replacement heap is created.

Frees from other threads needed no change. A remote free always changes the
block's state with a CAS and pushes it onto the owner's inbox with a release
CAS, whether the owner runs or not. The check that rejected frees to exited
owners is gone, which removes a load from the remote-free path.

## The reclamation worker

One detached thread per process, created on the first hand-over. It owns a
mutex-protected intrusive list of retired heaps, linked through a pointer in
the heap's identity cache line, so the heap object did not grow. Each pass,
holding its mutex, it does the following for every listed heap:

- If the inbox is empty, it skips the heap.
- Otherwise it drains the inbox (an acquire exchange, then the normal
  release with coalescing). Under the pool's lock it then returns spans that
  became empty to the pool's free-span list.
- If no span is left, it marks the heap recyclable and unlinks it.

It waits 1 ms between passes while passes find work, doubling the wait to at
most 50 ms when they do not. It sleeps on a condition variable while the list
is empty. Memory freed into a retired heap is therefore reclaimed within one
interval: 1 ms under steady activity, about 50 ms after an idle period.

Spans come from the pool's arena and are never released to the system
individually. Returned spans are taken by any thread that needs one before a
new span is carved.

## Concurrency guarantees

| Requirement | How it holds |
| --- | --- |
| The owner finishes all local heap mutations before the worker starts | The owner's last operations happen before it unlocks the worker's mutex in step 4. The worker reads a heap only inside a pass, which starts by locking that mutex: everything the owner did happens before everything the worker does. |
| No other thread touches the free lists concurrently | Active: only the owner. Retired: only the worker (the exiting thread has no cached heap, and its frees go to the inbox). Recyclable: nobody. Adoption happens under the pool lock after the worker marked the heap recyclable under the same lock. |
| A remote free racing with retirement is published safely | It is pushed onto the inbox, which the owner's final drain may or may not see. Whatever remains is drained by the worker. A block being freed is not free yet, so its span is not returned in step 2. |
| Remote frees after retirement are accepted, none is lost | The inbox is preserved across the hand-over. Every pushed block is counted as received before the push and is drained exactly once. |
| Only one thread drains an inbox at any moment | The owner drains only before step 4; the worker drains only after it, and only for heaps in its list. |
| A heap is not reused while a producer still refers to it | A producer refers to a heap only while freeing one of its blocks. That block counts as live (allocated, or pending until drained), so its span is not empty and the heap is not recyclable. Recyclable means no span, which implies no allocated or pending block. Recycling waits for the drain, which happens after the producer's push completed. Checked by an engine test that stops a free between its state change and its push. |
| Pool destruction coordinates with the worker | The worker holds its mutex for a whole pass. The pool destructor first leaves the registry, so no exiting thread can hand over a new heap, then unlinks its heaps under that mutex. After that the worker cannot reach the pool. `verify_integrity()` holds the mutex too. |
| Late `thread_local` destructors cannot regain the free lists | The exiting thread's cache is cleared and the exiting mark stops heap lookup and creation; frees go through inboxes, allocations fail. |

Lock order: the registry mutex, then the worker's mutex, then a pool's mutex.
Thread exit holds the registry mutex while handing over, and releases the
pool's lock before taking the worker's.

No atomic operation was added. The hand-over relies on the existing mutexes
and the existing `status` field, published with a release store under the
pool lock, and remote producers do not read the status at all.

## Statistics

- Counters belong to the heap object and survive retirement and reuse. The
  worker updates a retired heap's counters as its owner would.
- Remote frees are counted by the receiving heap, so frees into retired heaps
  are counted once.
- Frees a thread makes in its own late `thread_local` destructors count as
  remote, since they go through the inbox.
- Until the worker processes them, frees of a retired heap's blocks count as
  `pending_remote_frees` and their memory as live.
- `retired_thread_heap_count` is new.
- The documented identities hold once reclamation is done: no pending frees
  and no retired heaps.

## Tests

`tests/test_retirement.cpp` (public API; reclamation is awaited by polling
`stats()` until no heap is retired and no free is pending):

| Scenario | Test |
| --- | --- |
| A worker allocates, exits, another thread frees its blocks | `blocks_outlive_their_thread_and_are_freed_by_another` (2000 blocks up to 100 KB, freed in two halves, patterns checked after the exit) |
| Several retired heaps receive concurrent remote frees | `several_retired_heaps_receive_concurrent_remote_frees` (6 heaps x 3000 blocks, 4 freers, shuffled) |
| The last allocation is freed while the worker drains | `final_allocation_freed_while_the_collector_drains` (30 rounds; 599 frees, a random 0-3 ms pause, then the last; the heap must be reused every round) |
| A `thread_local` destructor frees after retirement | `thread_local_destructor_frees_after_retirement` |
| Allocation during post-retirement `thread_local` destruction fails | `allocation_after_retirement_fails` (`allocate()` throws `std::bad_alloc`; `try_allocate()`, aligned, oversized and pmr requests fail; a pool the thread never used refuses too; no heap is created) |
| Empty spans return to the pool and are reused | `empty_spans_of_retired_heaps_are_reused` (6 spans returned and taken by a new thread without carving) |
| Heap metadata is not reused while publications are in flight | `heaps_are_not_recycled_while_frees_are_in_flight` (producers free retired blocks while a churn of new threads adopts heaps as soon as they are reusable) and the engine test `a_span_with_a_free_in_flight_is_not_released` |
| Thousands of short-lived threads do not grow heap metadata | `thousands_of_short_lived_threads_keep_metadata_bounded` (2000 threads in batches of 40; heaps, spans and reserved bytes stay at their first-batch values) |
| Pool destruction coordinates with the worker | `pool_destruction_coordinates_with_the_worker` (100 pools destroyed while their retired heaps hold blocks and frees are being drained) |
| Statistics across retirement, draining and reuse | `statistics_across_retirement_draining_and_recycling` |
| Randomized exit and free timing | `randomized_retirement_stress` (300 threads, at most 6 alive, random lifetimes; 2 freers with random pauses) |

Updated tests:
- The misuse scenario "free after the owner exited" is gone (now valid).
- The thread tests that shared blocks with an exiting owner now also free
  them after the exit.

Results (local, GCC 13.3 and Clang 18, warnings as errors):

| Configuration | Result |
| --- | --- |
| GCC Debug, GCC Release, Clang Debug, Clang Release | 20/20 tests |
| GCC ASan + UBSan, Clang ASan + UBSan | 22/22 tests |
| Clang TSan | 19/19 tests |
| Repeated runs | `test_retirement`: TSan 9/9, ASan 4/4; `test_threads`: TSan 8/8 |
| Shared library, install, `find_package` consumer | 20/20 tests, consumer runs |
| Export without `.git` | 20/20 tests |

## Measurements

**Single thread**, paired comparison with the commit before this feature
(`9faf541`): five processes of 15 alternating iterations each, full-scale
traces, local Intel machine. Time ratio after / before, median over
processes [range]:

| Workload | allocation | deallocation | combined |
| --- | --- | --- | --- |
| small | 1.005 [0.96-1.02] | 0.983 [0.97-1.02] | 0.990 [0.96-1.02] |
| medium | 1.001 [0.99-1.03] | 0.986 [0.98-1.02] | 0.992 [0.99-1.05] |
| mixed | 0.985 [0.98-1.04] | 0.995 [0.99-1.02] | 0.993 [0.98-1.02] |
| lifo | 1.036 [0.93-1.18] | 1.036 [0.90-1.17] | 1.038 [0.92-1.16] |
| random | 0.983 [0.97-1.13] | 1.008 [0.98-1.39] | 0.994 [0.98-1.27] |
| bursts | 0.976 [0.95-0.99] | 0.991 [0.98-1.00] | 0.989 [0.97-0.99] |
| long_short | 1.025 [1.01-1.13] | 0.994 [0.99-1.17] | 1.000 [0.99-1.15] |
| fragmentation | 0.990 [0.96-1.07] | 0.999 [0.98-1.04] | 1.002 [0.97-1.05] |
| reuse | 0.953 [0.85-1.10] | 0.996 [0.99-1.00] | 0.985 [0.94-1.04] |

All within the noise floor of the method: a comparison of identical builds
gives 0.96-1.01, and `lifo` 0.87-1.10 across processes. The hot paths are
unchanged.

**Multithreaded** (`nanoalloc_mtbench`, 4 threads, 10 alternating runs per
build): combined throughput medians of 116 (before) and 132 (after) Mops/s
at 0% remote frees, and 29.9 and 29.9 Mops/s at 100%. Both builds switch
between the same two speed modes of the machine, so the distributions
overlap completely; no difference can be claimed either way.

**Reclamation** (2000 threads in 50 batches of 40; each thread allocates
48 B, 3000 B and 50 KB blocks and exits; another thread frees them):

| Measure | Value |
| --- | --- |
| Retired heaps before the frees | 40 per batch |
| Heaps carved over 2000 threads | 40 (all reused) |
| Spans carved | 40 (after each batch: 40 free spans) |
| Reserved bytes | 40.13 MiB after the first batch and after the last |
| Time from the last free to "no retired heap" | median 50.2 ms, max 50.6 ms (the worker was in its 50 ms idle back-off) |
| Final state | 6000 allocations, 6000 deallocations, all remote, nothing live, `verify_integrity()` passes |

## Limitations

- **Asynchronous reclamation.** Memory freed into a retired heap is
  reclaimed by the worker within its polling interval (up to about 50 ms
  after an idle period). Until then it counts as live and pending. A
  heap with blocks that are never freed keeps its spans until the pool is
  destroyed, and the worker checks its inbox every 50 ms while it exists.
- **One worker thread.** The worker processes all pools in turn; a pass with
  many large inboxes delays the next one. Thread exits and pool destruction
  wait for the current pass to finish.
- **No allocation in late `thread_local` destructors**, in any pool, once
  the thread's exit has been processed. A thread that never allocated from
  any pool before its exit hook ran is not affected.
- **Process exit.** The worker thread is detached. If the main thread
  retires a heap at exit, the worker is started then; it stops with the
  process. Pools destroyed during static destruction coordinate with it
  through its mutex. If a pool is destroyed after the process has already
  terminated its other threads (for example a static pool in a Windows DLL
  unloaded during process exit) and the worker was stopped inside a pass,
  the pool's destructor waits forever for the worker's mutex.
- If the worker thread cannot be started (`std::thread` throws), retired
  heaps stay registered and their memory stays reserved until a later
  hand-over starts the worker.
