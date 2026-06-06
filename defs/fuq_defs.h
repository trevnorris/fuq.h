#ifndef FUQ_DEFS_
#define FUQ_DEFS_

/**
 * Synchronization layer for fuq.
 *
 * fuq publishes exactly two pointers across the producer/consumer boundary:
 * the queue tail (producer -> consumer) and the free-list tail (consumer ->
 * producer). Each is published with a release store and observed with an
 * acquire load so that everything written before the publish is visible to
 * the thread that observes it. The pooled-slab counter is a relaxed atomic;
 * it only gates a soft memory cap, so a slightly stale value is harmless, but
 * it must never tear or lose an update.
 *
 * Two back ends provide that contract:
 *
 *   1. C11 <stdatomic.h> / C++ <atomic>  - correct by construction on every
 *      architecture. Used automatically when compiling as C11 (or later) with
 *      atomics, or as C++11 (or later).
 *
 *   2. Hand-rolled barriers - for pre-C11 C, pre-C++11 C++, and compilers
 *      without atomics (e.g. older MSVC). The per-architecture fences live in
 *      the fuq_*.h headers and are placed on the correct side of each
 *      load/store here.
 *
 * Define FUQ_NO_C11_ATOMICS before including fuq.h to force the barrier
 * fallback even when atomics are available. The barrier path is validated for
 * functional correctness only: it is not race-clean under the C/C++ memory
 * model and cannot be checked by ThreadSanitizer, so prefer an atomic back end
 * for concurrent use. The test suite uses this switch to exercise the fallback.
 */

/* Compiling as C++11 or later: use std::atomic. MSVC reports an old
 * __cplusplus (199711L) unless /Zc:__cplusplus is set, so also consult
 * _MSVC_LANG, which always carries the real language version. */
#if !defined(FUQ_NO_C11_ATOMICS) && defined(__cplusplus) &&                   \
    ((__cplusplus >= 201103L) ||                                             \
     (defined(_MSVC_LANG) && (_MSVC_LANG >= 201103L)))
# define FUQ_USE_CPP_ATOMICS 1
#else
# define FUQ_USE_CPP_ATOMICS 0
#endif

/* Compiling as C11 or later with atomics: use <stdatomic.h>. */
#if !defined(FUQ_NO_C11_ATOMICS) && !defined(__cplusplus) &&                  \
    !defined(__STDC_NO_ATOMICS__) &&                                          \
    defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
# define FUQ_USE_C11_ATOMICS 1
#else
# define FUQ_USE_C11_ATOMICS 0
#endif

#if FUQ_USE_CPP_ATOMICS
# define FUQ_BACKEND_NAME "C++ atomics"
#elif FUQ_USE_C11_ATOMICS
# define FUQ_BACKEND_NAME "C11 atomics"
#else
# define FUQ_BACKEND_NAME "barrier fallback"
#endif


#if FUQ_USE_CPP_ATOMICS

/* ---- C++ std::atomic back end -------------------------------------------- */

#include <atomic>

#define FUQ_ATOMIC(type)     std::atomic<type>
#define FUQ_ATOMIC_INT       std::atomic<int>

#define FUQ_INIT(ptr, val)                                                   \
  std::atomic_store_explicit((ptr), (val), std::memory_order_relaxed)

/* Acquire-load / release-store of a published pointer. */
#define FUQ_LOAD_ACQ(ptr)                                                    \
  std::atomic_load_explicit((ptr), std::memory_order_acquire)
#define FUQ_STORE_REL(ptr, val)                                              \
  std::atomic_store_explicit((ptr), (val), std::memory_order_release)

/* Relaxed access to a pointer owned (for writing) by the current thread. */
#define FUQ_LOAD_RLX(ptr)                                                    \
  std::atomic_load_explicit((ptr), std::memory_order_relaxed)
#define FUQ_STORE_RLX(ptr, val)                                              \
  std::atomic_store_explicit((ptr), (val), std::memory_order_relaxed)

/* Relaxed counter ops for the pooled-slab count. */
#define FUQ_CTR_LOAD(ptr)                                                    \
  std::atomic_load_explicit((ptr), std::memory_order_relaxed)
#define FUQ_CTR_ADD(ptr, n)                                                  \
  ((void) std::atomic_fetch_add_explicit((ptr), (n), std::memory_order_relaxed))
#define FUQ_CTR_SUB(ptr, n)                                                  \
  ((void) std::atomic_fetch_sub_explicit((ptr), (n), std::memory_order_relaxed))

#elif FUQ_USE_C11_ATOMICS

/* ---- C11 atomics back end ------------------------------------------------ */

#include <stdatomic.h>

#define FUQ_ATOMIC(type)     _Atomic(type)
#define FUQ_ATOMIC_INT       _Atomic int

#define FUQ_INIT(ptr, val)   atomic_init((ptr), (val))

/* Acquire-load / release-store of a published pointer. */
#define FUQ_LOAD_ACQ(ptr)    atomic_load_explicit((ptr), memory_order_acquire)
#define FUQ_STORE_REL(ptr, val)                                              \
  atomic_store_explicit((ptr), (val), memory_order_release)

/* Relaxed access to a pointer owned (for writing) by the current thread. */
#define FUQ_LOAD_RLX(ptr)    atomic_load_explicit((ptr), memory_order_relaxed)
#define FUQ_STORE_RLX(ptr, val)                                              \
  atomic_store_explicit((ptr), (val), memory_order_relaxed)

/* Relaxed counter ops for the pooled-slab count. */
#define FUQ_CTR_LOAD(ptr)    atomic_load_explicit((ptr), memory_order_relaxed)
#define FUQ_CTR_ADD(ptr, n)                                                  \
  ((void) atomic_fetch_add_explicit((ptr), (n), memory_order_relaxed))
#define FUQ_CTR_SUB(ptr, n)                                                  \
  ((void) atomic_fetch_sub_explicit((ptr), (n), memory_order_relaxed))

#else

/* ---- Hand-rolled barrier back end ---------------------------------------- */

/* Pull in fuq__read_barrier() / fuq__write_barrier() for this target. */
#if defined(_MSC_VER)
#include "fuq_win.h"
#elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__) || \
      defined(__powerpc64__) || defined(__ppc64__) || defined(__PPC64__)
#include "fuq_ppc.h"
#elif defined(__aarch64__)
#include "fuq_arm64.h"
#elif defined(__i386) || defined(_M_IX86) || \
      defined(__x86_64__) || defined(_M_X64)
#include "fuq_x86_32_64.h"
#elif (__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)
#include "fuq_gnuc_generic.h"
#else
#error "fuq: no supported atomics or memory-barrier back end for this build"
#endif

#define FUQ_ATOMIC(type)     type volatile
#define FUQ_ATOMIC_INT       int volatile

#define FUQ_INIT(ptr, val)   do { *(ptr) = (val); } while (0)

/*
 * Release store: the write barrier sits *before* the publishing store so that
 * every prior write is visible once the new pointer becomes observable.
 * Acquire load: the read barrier sits *after* the load so that no dependent
 * read is hoisted above the load of the published pointer.
 */
#define FUQ_STORE_REL(ptr, val)                                              \
  do { fuq__write_barrier(); *(ptr) = (val); } while (0)

#define FUQ_STORE_RLX(ptr, val)   do { *(ptr) = (val); } while (0)
#define FUQ_LOAD_RLX(ptr)         (*(ptr))

#if defined(__GNUC__)

/* Statement expression keeps the load type-correct (no aliasing through
 * void*) and places the acquire barrier immediately after the load. */
#define FUQ_LOAD_ACQ(ptr)                                                    \
  __extension__ ({                                                           \
    __typeof__(*(ptr)) fuq__v = *(ptr);                                      \
    fuq__read_barrier();                                                     \
    fuq__v;                                                                  \
  })

#define FUQ_CTR_LOAD(ptr)    (*(ptr))
#define FUQ_CTR_ADD(ptr, n)  ((void) __sync_add_and_fetch((ptr), (n)))
#define FUQ_CTR_SUB(ptr, n)  ((void) __sync_sub_and_fetch((ptr), (n)))

#elif defined(_MSC_VER)

/* MSVC (best effort, not exercised by the test suite on this platform).
 * Under the default /volatile:ms a volatile read carries acquire and a
 * volatile write carries release semantics; the explicit barrier reinforces
 * the load. MSVC does not enable strict-aliasing optimizations, so routing
 * the access through void* is safe here. */
#include <intrin.h>

static fuq__inline void* fuq__load_acq_ptr(void* volatile* ptr) {
  void* val = *ptr;
  fuq__read_barrier();
  return val;
}
#define FUQ_LOAD_ACQ(ptr)    fuq__load_acq_ptr((void* volatile*) (ptr))

#define FUQ_CTR_LOAD(ptr)    (*(ptr))
#define FUQ_CTR_ADD(ptr, n)                                                  \
  ((void) _InterlockedExchangeAdd((volatile long*) (ptr), (long) (n)))
#define FUQ_CTR_SUB(ptr, n)                                                  \
  ((void) _InterlockedExchangeAdd((volatile long*) (ptr), -(long) (n)))

#else
#error "fuq: barrier back end requires a GCC/Clang or MSVC compatible compiler"
#endif

#endif  /* FUQ_USE_C11_ATOMICS */

#endif  /* FUQ_DEFS_ */
