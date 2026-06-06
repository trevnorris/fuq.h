# fuq.h

## Overview

`fuq` stands for a <b>F</b>undamentally <b>U</b>nstable <b>Q</b>ueue. It is a
thread safe, lock-free SPSC (single producer single consumer) queue written in
C as a single header.

This code started as an academic experiment and eventually turned into a
working API. The name is a nod to how hard a lock-free queue is to get right;
the cross-thread synchronization is now built on C11 `<stdatomic.h>` or C++
`std::atomic` (with a hand-rolled memory-barrier fallback for older toolchains),
and the repository ships a test suite covering every back end — ThreadSanitizer
on the atomic back ends, plus ASan/UBSan and valgrind. See [Testing](#testing).

Being SPSC means only a single thread can push items onto the queue, and only
one thread can shift items from the queue. Ownership of which thread does each
operation can be transferred between threads, but take special care that only a
single thread performs each operation at any given time.

There is the case of a "false negative": an item can be in the process of being
pushed while the consumer shifts and receives `NULL`, indicating the queue is
empty when it is about to not be. This is inherent to a lock-free SPSC queue
and is harmless — the consumer simply tries again.


## API

[`fuq_queue_t`](#fuq_queue_t)<br>
[`fuq_init`](#void-fuq_initfuq_queue_t-queue)<br>
[`fuq_enqueue`](#void-fuq_enqueuefuq_queue_t-queue-void-arg)<br>
[`fuq_dequeue`](#void-fuq_dequeuefuq_queue_t-queue)<br>
[`fuq_empty`](#int-fuq_emptyfuq_queue_t-queue)<br>
[`fuq_dispose`](#void-fuq_disposefuq_queue_t-queue)

The API is kept as minimal and straightforward as possible. The operations were
once named `fuq_push` / `fuq_shift` after the
[JS Array](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Array)
methods; they are now `fuq_enqueue` / `fuq_dequeue`.


##### `fuq_queue_t`

The memory construct that holds all the information about an individual queue.
For convenience this `struct` is `typedef`'d.

To prevent unnecessary memory allocations the queue stores its items in slabs.
Each slab holds `FUQ_ARRAY_SIZE` pointers (default `4095`) plus one final slot
that links to the next slab, rounding the allocation out to 4096 pointers —
32 KiB on a 64-bit target. Define `FUQ_ARRAY_SIZE` before including `fuq.h` to
override it.

Further allocation is avoided by pooling slabs that are no longer in use. In
order to stay lock-free these pooled slabs are **not** shared between queue
instances; each queue keeps its own small free list. So remember that
[properly disposing](#void-fuq_disposefuq_queue_t-queue) of the queue at end of
life is important to release them.

To bound how much a single instance retains, at most `FUQ_MAX_STOR` slabs
(default `4`, i.e. up to 128 KiB at the default slab size) are pooled; beyond
that, freed slabs are returned to the allocator. Define `FUQ_MAX_STOR` before
including `fuq.h` to override it.


##### `void fuq_init(fuq_queue_t* queue)`

Pass an uninitialized `fuq_queue_t` instance. Instances that have been passed to
[`fuq_dispose`](#void-fuq_disposefuq_queue_t-queue) are considered
uninitialized and may be re-initialized.

On initialization the initial slab is allocated and the cursors are set.


##### `void fuq_enqueue(fuq_queue_t* queue, void* arg)`

Place a single `void* arg` onto the queue. May be called at any time from the
producer thread without locking.


##### `void* fuq_dequeue(fuq_queue_t* queue)`

Returns the next item in the queue, or `NULL` if the queue is empty. Note that
`NULL` is also a perfectly valid value to enqueue; doing so just makes the
queue indistinguishable from empty for that item (see the "false negative"
note above), so prefer non-`NULL` payloads or pair them with
[`fuq_empty`](#int-fuq_emptyfuq_queue_t-queue).


##### `int fuq_empty(fuq_queue_t* queue)`

Returns non-zero if the queue currently appears empty. Subject to the same
false-negative property as `fuq_dequeue`.


##### `void fuq_dispose(fuq_queue_t* queue)`

Frees all allocated slabs, including the pooled free list. Once this runs the
queue must be considered uninitialized; no other operation may run on it
concurrently. Note that `fuq_dispose` frees the queue's slabs, not the items
still stored in them — drain or otherwise reclaim those first if they own
heap memory.


## Building

`fuq.h` is header-only; just `#include "fuq.h"`.

The synchronization back end is selected automatically:

* **C11 atomics** (`<stdatomic.h>`) when compiling as C11 or later with atomics
  available.
* **C++ atomics** (`std::atomic`) when compiling as C++11 or later.
* **Hand-rolled barriers** otherwise — pre-C11 C, pre-C++11 C++, or compilers
  without atomics (e.g. older MSVC). The per-architecture fences live in
  `defs/` (x86/x86-64, ARM64, PPC, and a generic GCC/Clang path).

The two atomic back ends are correct by construction on every supported
architecture and are verified race-free under ThreadSanitizer. The barrier
fallback is validated functionally only — it is not race-clean under the
language memory model and cannot be modeled by ThreadSanitizer — so prefer an
atomic back end for concurrent use. Define `FUQ_NO_C11_ATOMICS` before including
`fuq.h` to force the barrier fallback (functional-only).

Define `DEBUG` to enable a fatal out-of-memory check on allocation.


## Testing

A test suite lives in `tests/` and is driven by the `Makefile`:

```
make check          # build and run every configuration
make check-basic    # single-threaded functional tests
make check-pool     # slab recycling / leak tests (malloc intercepted)
make check-spsc     # concurrent producer/consumer stress (C)
make check-cpp      # C++: std::atomic concurrent stress + barrier-path smoke
```

`make check` runs the functional, pool and concurrency tests across the C11,
C++ and barrier-fallback back ends, on gcc, clang and g++, under ASan/UBSan,
valgrind, and ThreadSanitizer. The concurrent test verifies strict FIFO
ordering of millions of items and a watchdog turns any lost item into a failure
rather than a hang. Increase the load with, for example,
`make check-spsc SPSC_BIG=50000000`.

The C11 and C++ atomic back ends are verified race-free under ThreadSanitizer.

> Tested primarily on x86-64 Linux. The ARM64 and PPC barrier fallbacks are
> written to the standard acquire/release pattern (`lwsync` on PPC, `dmb` on
> ARM64) but are not exercised on those architectures in this repository's CI;
> ThreadSanitizer cannot model the inline-asm fences, so the barrier path is
> validated functionally rather than with TSan.


## Example

A complete, runnable example is in
[`example/sporadic.c`](example/sporadic.c) (`make example`). A minimal sketch:

```c
#include "./fuq.h"
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define ITER 10000000

static void* task_runner(void* arg) {
  fuq_queue_t* queue = (fuq_queue_t*) arg;
  uintptr_t i;
  /* Push values onto the queue as fast as possible. */
  for (i = 1; i < ITER; i++)
    fuq_enqueue(queue, (void*) i);
  return NULL;
}

int main(void) {
  fuq_queue_t queue;
  pthread_t thread;
  uint64_t sum = 0;
  uint64_t check = 0;
  void* tmp;
  uintptr_t i;

  fuq_init(&queue);
  assert(pthread_create(&thread, NULL, task_runner, &queue) == 0);

  /* Notice i starts at 1, because 0x0 == NULL. */
  for (i = 1; i < ITER; i++) {
    check += i;                         /* so the final sum can be verified */
    if (NULL != (tmp = fuq_dequeue(&queue)))
      sum += (uint64_t) (uintptr_t) tmp;
  }

  assert(pthread_join(thread, NULL) == 0);

  /* Drain whatever remains. */
  while (NULL != (tmp = fuq_dequeue(&queue)))
    sum += (uint64_t) (uintptr_t) tmp;

  fprintf(stderr, "check: %" PRIu64 "\nsum:   %" PRIu64 "\n", check, sum);
  assert(sum == check);

  fuq_dispose(&queue);
  return 0;
}
```
