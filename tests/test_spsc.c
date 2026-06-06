/* Expose POSIX (pthreads, alarm, SIGALRM, write, _exit) under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

/**
 * Concurrent single-producer / single-consumer tests for fuq.
 *
 * A producer thread enqueues 1, 2, 3, ... N while a consumer dequeues. Because
 * the queue is FIFO, the sequence of non-NULL values the consumer observes
 * must be exactly 1, 2, 3, ... N: any reordering, duplication, loss or torn
 * pointer shows up immediately as a mismatch, and any lost item trips the
 * watchdog instead of hanging.
 *
 * Run this build under ThreadSanitizer on the C11 atomics path to verify the
 * synchronization is data-race free. (The hand-rolled barrier fallback uses
 * inline-asm fences that TSan cannot model, so it is exercised without TSan.)
 *
 * Usage: test_spsc [iters]
 */

#include "../fuq.h"

#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int failures = 0;
static int mismatches_reported = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      failures++;                                                            \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                        \
  } while (0)


static void on_timeout(int sig) {
  (void) sig;
  const char msg[] = "  FAIL: watchdog timeout - items lost or deadlocked\n";
  /* write() is async-signal-safe; fprintf is not. */
  if (write(2, msg, sizeof(msg) - 1)) { /* ignore */ }
  _exit(1);
}


typedef struct {
  fuq_queue_t* queue;
  uintptr_t count;
} producer_arg_t;


static void* producer(void* arg) {
  producer_arg_t* pa = (producer_arg_t*) arg;
  uintptr_t i;
  for (i = 1; i <= pa->count; i++)
    fuq_enqueue(pa->queue, (void*) i);
  return NULL;
}


/* Consume exactly n items from queue, asserting strict 1..n FIFO order.
 * Returns the sum of the values observed. */
static uint64_t consume_checked(fuq_queue_t* queue, uintptr_t n) {
  uintptr_t expected = 1;
  uintptr_t received = 0;
  uint64_t sum = 0;

  while (received < n) {
    void* v = fuq_dequeue(queue);
    if (v == NULL)
      continue; /* documented false negative: producer mid-push */
    if ((uintptr_t) v != expected) {
      failures++;
      if (mismatches_reported++ < 10)
        fprintf(stderr, "  FAIL: out of order: got %ju expected %ju\n",
                (uintmax_t) (uintptr_t) v, (uintmax_t) expected);
    }
    sum += (uint64_t) (uintptr_t) v;
    expected++;
    received++;
  }
  return sum;
}


/* One producer thread, consumer on the main thread, single queue. */
static void test_single_queue(uintptr_t n) {
  fuq_queue_t q;
  pthread_t thread;
  producer_arg_t pa;
  uint64_t sum;
  uint64_t want = (uint64_t) n * (n + 1) / 2;

  fuq_init(&q);
  pa.queue = &q;
  pa.count = n;
  CHECK(pthread_create(&thread, NULL, producer, &pa) == 0);

  sum = consume_checked(&q, n);

  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(sum == want);
  CHECK(fuq_empty(&q));
  CHECK(fuq_dequeue(&q) == NULL);
  fuq_dispose(&q);
}


/* Two producer threads each feeding their own queue, both drained from the
 * main thread. Stresses two concurrent free-lists at once. */
static void test_two_queues(uintptr_t n) {
  fuq_queue_t q0, q1;
  pthread_t t0, t1;
  producer_arg_t a0, a1;
  uintptr_t exp0 = 1, exp1 = 1, got0 = 0, got1 = 0;
  uint64_t sum0 = 0, sum1 = 0;
  uint64_t want = (uint64_t) n * (n + 1) / 2;

  fuq_init(&q0);
  fuq_init(&q1);
  a0.queue = &q0; a0.count = n;
  a1.queue = &q1; a1.count = n;
  CHECK(pthread_create(&t0, NULL, producer, &a0) == 0);
  CHECK(pthread_create(&t1, NULL, producer, &a1) == 0);

  while (got0 < n || got1 < n) {
    if (got0 < n) {
      void* v = fuq_dequeue(&q0);
      if (v != NULL) {
        if ((uintptr_t) v != exp0) failures++;
        sum0 += (uint64_t) (uintptr_t) v; exp0++; got0++;
      }
    }
    if (got1 < n) {
      void* v = fuq_dequeue(&q1);
      if (v != NULL) {
        if ((uintptr_t) v != exp1) failures++;
        sum1 += (uint64_t) (uintptr_t) v; exp1++; got1++;
      }
    }
  }

  CHECK(pthread_join(t0, NULL) == 0);
  CHECK(pthread_join(t1, NULL) == 0);
  CHECK(sum0 == want);
  CHECK(sum1 == want);
  CHECK(fuq_empty(&q0));
  CHECK(fuq_empty(&q1));
  fuq_dispose(&q0);
  fuq_dispose(&q1);
}


int main(int argc, char** argv) {
  uintptr_t iters = (argc > 1) ? (uintptr_t) strtoull(argv[1], NULL, 10)
                               : 2000000;
  int round;

  signal(SIGALRM, on_timeout);
  alarm(120);

  fprintf(stderr, "test_spsc (%ju iters, %s)\n",
          (uintmax_t) iters, FUQ_BACKEND_NAME);

  /* A few rounds to shake out rare interleavings. */
  for (round = 0; round < 3; round++)
    test_single_queue(iters);

  test_two_queues(iters);

  alarm(0);
  fprintf(stderr, "  %d failures\n", failures);
  return failures == 0 ? 0 : 1;
}
