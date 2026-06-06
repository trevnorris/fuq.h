/**
 * Slab-pool tests for fuq.
 *
 * Regression guard for the inverted free-list comparison: before the fix,
 * fuq__free_array() always called free() and max_stor never left 0, so the
 * pool described in the README did nothing. These tests intercept malloc/free
 * (via the linker's --wrap) and assert that slabs are actually recycled, that
 * the pool is bounded by FUQ_MAX_STOR, and that nothing leaks.
 *
 * Build with: -Wl,--wrap=malloc,--wrap=free   (do not combine with ASan,
 * which provides its own malloc).
 */

#define FUQ_ARRAY_SIZE 15
#define FUQ_MAX_STOR   4
#define TEST_SLAB      15
#define TEST_MAX_STOR  4

#include "../fuq.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern void* __real_malloc(size_t);
extern void  __real_free(void*);

/* volatile: malloc() is redirected to __wrap_malloc only at link time, so the
 * compiler does not know these calls touch the counters and would otherwise
 * cache reads of them across allocations under optimization. */
static volatile long n_malloc = 0;
static volatile long n_free = 0;

void* __wrap_malloc(size_t n) { n_malloc++; return __real_malloc(n); }
void  __wrap_free(void* p)    { if (p) n_free++; __real_free(p); }

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      failures++;                                                            \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                        \
  } while (0)

#define PVAL(n) ((void*) (uintptr_t) (n))


static void fill(fuq_queue_t* q, uintptr_t n) {
  uintptr_t i;
  for (i = 0; i < n; i++)
    fuq_enqueue(q, PVAL(i + 1));
}

static void drain(fuq_queue_t* q) {
  while (!fuq_empty(q))
    (void) fuq_dequeue(q);
}


/* A full fill-then-drain pools slabs up to the cap and reuses them next
 * round, so later rounds issue strictly fewer mallocs. */
static void test_recycle_and_cap(void) {
  fuq_queue_t q;
  uintptr_t per_round = TEST_SLAB * 20;
  long m0, m1;
  long base_m, base_f;

  base_m = n_malloc;
  base_f = n_free;

  fuq_init(&q);

  /* Round 0: cold pool, every slab is malloc'd. */
  m0 = n_malloc;
  fill(&q, per_round);
  drain(&q);
  m0 = n_malloc - m0;

  /* Original bug: pool stayed empty forever. It must now be filled to cap. */
  CHECK((int) q.max_stor == TEST_MAX_STOR);

  /* Round 1: warm pool, the first TEST_MAX_STOR slabs come from the pool. */
  m1 = n_malloc;
  fill(&q, per_round);
  drain(&q);
  m1 = n_malloc - m1;

  CHECK(m1 < m0);                       /* recycling happened */
  CHECK(m1 == m0 - TEST_MAX_STOR);      /* exactly cap fewer allocations */
  CHECK((int) q.max_stor == TEST_MAX_STOR);

  fuq_dispose(&q);

  /* Everything allocated across the queue's life was freed. */
  CHECK(n_malloc - base_m == n_free - base_f);
}


/* The pool count never exceeds FUQ_MAX_STOR, no matter how many slabs churn. */
static void test_cap_never_exceeded(void) {
  fuq_queue_t q;
  int round;
  fuq_init(&q);
  for (round = 0; round < 8; round++) {
    fill(&q, TEST_SLAB * 30);
    drain(&q);
    CHECK((int) q.max_stor <= TEST_MAX_STOR);
    CHECK((int) q.max_stor >= 0);
  }
  fuq_dispose(&q);
}


/* Oscillating across a single slab boundary with a warm pool should recycle
 * perfectly: zero further allocations and zero frees per cycle. This is the
 * strongest demonstration that the pool actually works. */
static void test_steady_state_no_alloc(void) {
  fuq_queue_t q;
  uintptr_t k = TEST_SLAB + 2; /* crosses exactly one boundary per cycle */
  int cycle;
  long m_before, f_before;

  fuq_init(&q);

  /* Warm the pool by churning a few slabs. */
  fill(&q, TEST_SLAB * 3);
  drain(&q);
  CHECK((int) q.max_stor >= 1);

  m_before = n_malloc;
  f_before = n_free;

  for (cycle = 0; cycle < 1000; cycle++) {
    fill(&q, k);
    drain(&q);
  }

  /* Pool absorbed all slab churn: no allocator traffic at all. */
  CHECK(n_malloc - m_before == 0);
  CHECK(n_free - f_before == 0);

  fuq_dispose(&q);
}


int main(void) {
  fprintf(stderr, "test_pool (%s)\n", FUQ_BACKEND_NAME);

  test_recycle_and_cap();
  test_cap_never_exceeded();
  test_steady_state_no_alloc();

  fprintf(stderr, "  %ld mallocs, %ld frees total, %d failures\n",
          n_malloc, n_free, failures);
  return failures == 0 ? 0 : 1;
}
