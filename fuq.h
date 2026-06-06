/**
 * fuq - a Fundamentally Unstable Queue
 *
 * fuq handles single consumer, single producer scenarios where only one
 * thread is pushing data to the queue and another is shifting data out.
 *
 * There is the case of a "false negative". Meaning an item can be in process
 * of being pushed into the queue while the other end is shifting data out
 * and receives NULL, indicating that the queue is empty.
 */

#ifndef FUQ_H_
#define FUQ_H_

#include <stdlib.h>  /* malloc, free, abort */
#if defined(DEBUG)
#include <stdio.h>   /* fprintf, fflush */
#endif

/* Prevent warnings when compiled with --std=gnu89 -pedantic */
#if defined (__STRICT_ANSI__) || defined (__GNUC_GNU_INLINE__)
# define fuq__inline __inline__
#else
# define fuq__inline inline
#endif

/* Selects the synchronization back end and pulls in <stdatomic.h> or the C++
 * <atomic> template header. Kept outside the extern "C" block below because
 * <atomic> must not be declared with C language linkage. */
#include "defs/fuq_defs.h"

#ifndef FUQ_ARRAY_SIZE
/* Number of usable slots in a slab. The slab holds FUQ_ARRAY_SIZE + 1
 * pointers; the final slot links to the next slab. The default rounds the
 * allocation out to 4096 pointers (32 KiB on a 64-bit target). Override by
 * defining FUQ_ARRAY_SIZE before including fuq.h. */
#define FUQ_ARRAY_SIZE 4095
#endif

#ifndef FUQ_MAX_STOR
/* Maximum number of unused slabs a single queue will pool for reuse before
 * releasing them back to the allocator. Override before including fuq.h. */
#define FUQ_MAX_STOR 4
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The last slot is reserved as a pointer to the next fuq__array. */
typedef void* fuq__array[FUQ_ARRAY_SIZE + 1];

typedef struct {
  fuq__array* head_array;
  fuq__array* tail_array;
  int head_idx;
  int tail_idx;
  /* Published queue cursor. The producer publishes tail; the consumer
   * observes it. head is only ever touched by the consumer. */
  void** head;
  FUQ_ATOMIC(void**) tail;
  /* Free list of unused slabs, run as a reverse SPSC channel: the consumer
   * publishes tail_stor, the producer observes it and pops from head_stor. */
  fuq__array* head_stor;
  FUQ_ATOMIC(fuq__array*) tail_stor;
  /* Number of fuq__array's currently pooled (relaxed counter). */
  FUQ_ATOMIC_INT max_stor;
} fuq_queue_t;


#if defined(DEBUG)
#define FUQ_CHECK_OOM(pntr)                                                   \
  do {                                                                        \
    if (NULL != pntr) continue;                                               \
    fprintf(stderr, "FATAL: OOM - %s:%i\n", __FILE__, __LINE__);              \
    fflush(stderr);                                                           \
    abort();                                                                  \
  } while (0)
#else
#define FUQ_CHECK_OOM(pntr)
#endif


static fuq__inline fuq__array* fuq__alloc_array(fuq_queue_t* queue) {
  fuq__array* array;
  fuq__array* tail_stor;

  tail_stor = (fuq__array*) FUQ_LOAD_ACQ(&queue->tail_stor);

  if (tail_stor == queue->head_stor) {
    array = (fuq__array*) malloc(sizeof(*array));
    FUQ_CHECK_OOM(array);
  } else {
    array = queue->head_stor;
    queue->head_stor = (fuq__array*) (*array)[1];
    FUQ_CTR_SUB(&queue->max_stor, 1);
  }

  return array;
}


static fuq__inline void fuq__free_array(fuq_queue_t* queue, fuq__array* array) {
  fuq__array* tail_stor;

  /* Pool is full: release the slab back to the allocator. */
  if (FUQ_CTR_LOAD(&queue->max_stor) >= FUQ_MAX_STOR) {
    free((void*) array);
    return;
  }

  (*array)[1] = NULL;
  tail_stor = (fuq__array*) FUQ_LOAD_RLX(&queue->tail_stor);
  (*tail_stor)[1] = array;
  FUQ_CTR_ADD(&queue->max_stor, 1);
  FUQ_STORE_REL(&queue->tail_stor, array);
}


static fuq__inline void fuq_init(fuq_queue_t* queue) {
  fuq__array* array;
  fuq__array* stor;

  array = (fuq__array*) malloc(sizeof(*array));
  FUQ_CHECK_OOM(array);
  stor = (fuq__array*) malloc(sizeof(*stor));
  FUQ_CHECK_OOM(stor);
  /* Initialize in case fuq_dispose() is called immediately after fuq_init(). */
  (*stor)[1] = NULL;

  queue->head_array = array;
  queue->tail_array = array;
  queue->head_idx = 0;
  queue->tail_idx = 0;
  queue->head = &(**array);
  FUQ_INIT(&queue->tail, &(**array));
  queue->head_stor = stor;
  FUQ_INIT(&queue->tail_stor, stor);
  FUQ_INIT(&queue->max_stor, 0);
}


static fuq__inline int fuq_empty(fuq_queue_t* queue) {
  void** tail;
  tail = (void**) FUQ_LOAD_ACQ(&queue->tail);
  return queue->head == tail;
}


static fuq__inline void fuq_enqueue(fuq_queue_t* queue, void* arg) {
  fuq__array* array;
  void** tail;

  /* The producer is the sole writer of tail, so a relaxed load is enough. */
  tail = (void**) FUQ_LOAD_RLX(&queue->tail);
  *tail = arg;
  queue->tail_idx += 1;

  if (FUQ_ARRAY_SIZE > queue->tail_idx) {
    FUQ_STORE_REL(&queue->tail, &((*queue->tail_array)[queue->tail_idx]));
    return;
  }

  array = fuq__alloc_array(queue);
  (*queue->tail_array)[queue->tail_idx] = (void*) array;
  queue->tail_array = array;
  queue->tail_idx = 0;
  FUQ_STORE_REL(&queue->tail, &(**array));
}


static fuq__inline void* fuq_dequeue(fuq_queue_t* queue) {
  fuq__array* next_array;
  void* ret;

  if (fuq_empty(queue))
    return NULL;

  ret = *queue->head;
  queue->head_idx += 1;
  queue->head = &((*queue->head_array)[queue->head_idx]);

  if (FUQ_ARRAY_SIZE > queue->head_idx)
    return ret;

  next_array = (fuq__array*) *queue->head;
  fuq__free_array(queue, queue->head_array);
  queue->head = &(**next_array);
  queue->head_array = next_array;
  queue->head_idx = 0;

  return ret;
}


/* Useful for cleanup at end of applications life to make valgrind happy. */
static fuq__inline void fuq_dispose(fuq_queue_t* queue) {
  void* next_array;

  while (queue->head_array != queue->tail_array) {
    next_array = (*queue->head_array)[FUQ_ARRAY_SIZE];
    free((void*) queue->head_array);
    queue->head_array = (fuq__array*) next_array;
  }

  free((void*) queue->head_array);

  if (NULL == queue->head_stor)
    return;

  do {
    next_array = (*queue->head_stor)[1];
    free((void*) queue->head_stor);
    queue->head_stor = (fuq__array*) next_array;
  } while (NULL != next_array);
}


#undef FUQ_ARRAY_SIZE
#undef FUQ_MAX_STOR
#undef FUQ_CHECK_OOM
#undef FUQ_ATOMIC
#undef FUQ_ATOMIC_INT
#undef FUQ_INIT
#undef FUQ_LOAD_ACQ
#undef FUQ_STORE_REL
#undef FUQ_LOAD_RLX
#undef FUQ_STORE_RLX
#undef FUQ_CTR_LOAD
#undef FUQ_CTR_ADD
#undef FUQ_CTR_SUB

#ifdef __cplusplus
}
#endif
#endif  /* FUQ_H_ */
