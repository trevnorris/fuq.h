/**
 * Single-threaded functional tests for fuq.
 *
 * Uses a small slab size by default so that slab allocation, linking, reuse
 * and disposal are exercised with modest item counts. Build with
 * -DTEST_SLAB_SIZE=4095 to run the same tests against the library default.
 *
 * Designed to be run under valgrind and ASan/UBSan to catch leaks and
 * memory errors in the slab bookkeeping.
 */

#ifndef TEST_SLAB_SIZE
#define TEST_SLAB_SIZE 7
#endif
#define FUQ_ARRAY_SIZE TEST_SLAB_SIZE

#include "../fuq.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define SLAB ((uintptr_t) TEST_SLAB_SIZE)

static int tests_run = 0;
static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    tests_run++;                                                             \
    if (!(cond)) {                                                           \
      failures++;                                                            \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                        \
  } while (0)

#define PVAL(n) ((void*) (uintptr_t) (n))


static void test_empty_new(void) {
  fuq_queue_t q;
  fuq_init(&q);
  CHECK(fuq_empty(&q));
  CHECK(fuq_dequeue(&q) == NULL);
  CHECK(fuq_dequeue(&q) == NULL);
  CHECK(fuq_empty(&q));
  fuq_dispose(&q);
}


static void test_single(void) {
  fuq_queue_t q;
  fuq_init(&q);
  fuq_enqueue(&q, PVAL(42));
  CHECK(!fuq_empty(&q));
  CHECK(fuq_dequeue(&q) == PVAL(42));
  CHECK(fuq_empty(&q));
  CHECK(fuq_dequeue(&q) == NULL);
  fuq_dispose(&q);
}


static void test_fifo_order(uintptr_t n) {
  fuq_queue_t q;
  uintptr_t i;
  fuq_init(&q);
  for (i = 1; i <= n; i++)
    fuq_enqueue(&q, PVAL(i));
  for (i = 1; i <= n; i++)
    CHECK(fuq_dequeue(&q) == PVAL(i));
  CHECK(fuq_empty(&q));
  CHECK(fuq_dequeue(&q) == NULL);
  fuq_dispose(&q);
}


/* Fill, fully drain, refill and drain again. Exercises the pooled-slab reuse
 * path and confirms the queue keeps FIFO order across a drain to empty. */
static void test_drain_refill(void) {
  fuq_queue_t q;
  uintptr_t i;
  uintptr_t n = SLAB * 6 + 3;
  fuq_init(&q);

  for (i = 1; i <= n; i++)
    fuq_enqueue(&q, PVAL(i));
  for (i = 1; i <= n; i++)
    CHECK(fuq_dequeue(&q) == PVAL(i));
  CHECK(fuq_empty(&q));

  for (i = n + 1; i <= 2 * n; i++)
    fuq_enqueue(&q, PVAL(i));
  for (i = n + 1; i <= 2 * n; i++)
    CHECK(fuq_dequeue(&q) == PVAL(i));
  CHECK(fuq_empty(&q));

  fuq_dispose(&q);
}


/* Push and pop in an uneven interleaved pattern so the head and tail cursors
 * cross slab boundaries at different times. A model counter verifies that the
 * exact FIFO sequence comes back out (single threaded => no false negatives). */
static void test_interleave(void) {
  fuq_queue_t q;
  uintptr_t enq_next = 1;
  uintptr_t deq_next = 1;
  uintptr_t total = SLAB * 50;
  int step;

  fuq_init(&q);

  while (deq_next <= total) {
    /* Enqueue a burst (bounded so we never exceed total). */
    for (step = 0; step < 5 && enq_next <= total; step++)
      fuq_enqueue(&q, PVAL(enq_next++));

    /* Dequeue a smaller burst; each non-NULL item must be the next in order. */
    for (step = 0; step < 3; step++) {
      void* v = fuq_dequeue(&q);
      if (v == NULL) {
        CHECK(deq_next == enq_next); /* only empty when fully drained */
        break;
      }
      CHECK(v == PVAL(deq_next));
      deq_next++;
    }
  }

  /* Drain whatever is left. */
  for (;;) {
    void* v = fuq_dequeue(&q);
    if (v == NULL)
      break;
    CHECK(v == PVAL(deq_next));
    deq_next++;
  }

  CHECK(deq_next - 1 == total);
  CHECK(fuq_empty(&q));
  fuq_dispose(&q);
}


/* Verify arbitrary pointer payloads (not just small integers) survive a
 * round-trip across slab boundaries, including their contents. */
static void test_pointer_payload(void) {
  fuq_queue_t q;
  uintptr_t n = SLAB * 4 + 1;
  uintptr_t i;
  fuq_init(&q);

  for (i = 0; i < n; i++) {
    uintptr_t* p = (uintptr_t*) malloc(sizeof(*p));
    *p = i * 7 + 1;
    fuq_enqueue(&q, p);
  }
  for (i = 0; i < n; i++) {
    uintptr_t* p = (uintptr_t*) fuq_dequeue(&q);
    CHECK(p != NULL);
    if (p != NULL) {
      CHECK(*p == i * 7 + 1);
      free(p);
    }
  }
  CHECK(fuq_empty(&q));
  fuq_dispose(&q);
}


/* dispose() on a queue that still holds items must free every slab (the
 * payloads here are non-heap, so valgrind should report zero leaks). */
static void test_dispose_partial(void) {
  fuq_queue_t q;
  uintptr_t i;
  fuq_init(&q);
  for (i = 1; i <= SLAB * 8 + 2; i++)
    fuq_enqueue(&q, PVAL(i));
  /* Drain only a portion, leaving multiple slabs live. */
  for (i = 1; i <= SLAB * 3; i++)
    CHECK(fuq_dequeue(&q) == PVAL(i));
  fuq_dispose(&q);
}


static void test_dispose_immediately(void) {
  fuq_queue_t q;
  fuq_init(&q);
  fuq_dispose(&q);
}


static void test_reinit(void) {
  fuq_queue_t q;
  int round;
  for (round = 0; round < 3; round++) {
    uintptr_t i;
    uintptr_t n = SLAB * 5 + round;
    fuq_init(&q);
    for (i = 1; i <= n; i++)
      fuq_enqueue(&q, PVAL(i));
    for (i = 1; i <= n; i++)
      CHECK(fuq_dequeue(&q) == PVAL(i));
    fuq_dispose(&q);
  }
}


int main(void) {
  fprintf(stderr, "test_basic (slab size %lu, %s)\n",
          (unsigned long) SLAB, FUQ_BACKEND_NAME);

  test_empty_new();
  test_single();
  test_fifo_order(1);
  test_fifo_order(SLAB - 1);     /* exactly one slot short of a slab */
  test_fifo_order(SLAB);         /* exactly one slab boundary */
  test_fifo_order(SLAB + 1);     /* just past a boundary */
  test_fifo_order(SLAB * 10 + 3);/* many slabs */
  test_drain_refill();
  test_interleave();
  test_pointer_payload();
  test_dispose_partial();
  test_dispose_immediately();
  test_reinit();

  fprintf(stderr, "  %d checks, %d failures\n", tests_run, failures);
  return failures == 0 ? 0 : 1;
}
