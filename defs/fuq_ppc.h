#ifndef FUQ_PPC_
#define FUQ_PPC_

/*
 * On PowerPC the acquire/release barrier for normal cacheable shared memory is
 * lwsync (it orders load-load, load-store and store-store; only store-load is
 * left unordered, which release/acquire does not need). eieio only orders
 * caching-inhibited / guarded (device) memory and isync is a speculation
 * barrier, so neither is a correct store-release for ordinary memory.
 *
 * Not exercised on PPC hardware in this repository's tests; see README.
 */

#if defined(__GNUC__)

static fuq__inline void fuq__read_barrier(void) {
  __asm__ __volatile__  ("lwsync":::"memory");
}

static fuq__inline void fuq__write_barrier(void) {
  __asm__ __volatile__  ("lwsync":::"memory");
}

#elif defined(__xlC__)

/* lwsync opcode 0x7c2004ac (see the PowerPC architecture / IBM developerworks
 * "powerpc.html" memory-barrier article). */
#pragma mc_func fuq__read_barrier  { "7c2004ac" }  /* lwsync */
#pragma mc_func fuq__write_barrier { "7c2004ac" }  /* lwsync */

#else
#error "No supported memory barrier options for this build"
#endif

#endif  /* FUQ_PPC_ */
