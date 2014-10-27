/**
 * Compiled using: -g -Wall -pthread --std=gnu89 -o sporadic -O3 sporadic.c
 */

#include "../fuq.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#define ITER 1e7


static void* task_runner(void* arg) {
  fuq_queue* queue;
  uint64_t i;

  queue = (fuq_queue*) arg;
  for (i = 1; i < ITER; i++) {
    fuq_push(queue, (void*) i);
  }
  return NULL;
}


int main(void) {
  fuq_queue queue0;
  fuq_queue queue1;
  pthread_t thread0;
  pthread_t thread1;
  uint64_t sum0 = 0;
  uint64_t sum1 = 0;
  uint64_t check = 0;
  void* tmp;
  uint64_t i;

  fuq_init(&queue0);
  fuq_init(&queue1);
  assert(pthread_create(&thread0, NULL, task_runner, &queue0) == 0);
  assert(pthread_create(&thread1, NULL, task_runner, &queue1) == 0);

  for (i = 1; i < ITER; i++) {
    check += i;
    if (NULL != (tmp = fuq_shift(&queue0)))
      sum0 += (uint64_t) tmp;
    if (NULL != (tmp = fuq_shift(&queue1)))
      sum1 += (uint64_t) tmp;
  }

  fprintf(stderr, "check: %" PRIu64 "\n", check);
  fprintf(stderr, "sum0:  %" PRIu64 "\n", sum0);
  fprintf(stderr, "sum1:  %" PRIu64 "\n", sum1);

  assert(pthread_join(thread0, NULL) == 0);
  assert(pthread_join(thread1, NULL) == 0);

  while (!fuq_empty(&queue0))
    sum0 += (uint64_t) fuq_shift(&queue0);
  while (!fuq_empty(&queue1))
    sum1 += (uint64_t) fuq_shift(&queue1);

  fprintf(stderr, "sum0:  %" PRIu64 "\n", sum0);
  fprintf(stderr, "sum1:  %" PRIu64 "\n", sum1);
  assert(sum0 == check);
  assert(sum1 == check);

  fuq_dispose(&queue0);
  fuq_dispose(&queue1);

  return 0;
}
