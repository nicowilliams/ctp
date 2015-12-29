
#include "atomics.h"

/*
 * XXX We've left Illumos atomics behind; rename symbols to avoid
 * conflicts.
 */

#ifdef HAVE___ATOMIC
/* Nothing to do */
#elif defined(HAVE___SYNC)
/* Nothing to do */
#elif defined(WIN32)
#include <windows.h>
#include <WinBase.h>
#elif defined(HAVE_INTEL_INTRINSICS)
#include <ia64intrin.h>
#elif defined(HAVE_PTHREAD)
#include <pthread.h>
static pthread_mutex_t atomic_lock = PTHREAD_MUTEX_INITIALIZER;
#elif defined(NO_THREADS)
/* Nothing to do */
#else
#error "Error: no acceptable implementation of atomic primitives is available"
#endif

uint32_t
atomic_inc_32_nv(volatile uint32_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    return __sync_fetch_and_add(p, 1) + 1;
#elif defined(WIN32)
    return InterlockedIncrement(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    return _InterlockedIncrement((volatile int32_t *)p);
#elif defined(HAVE_PTHREAD)
    uint32_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = ++(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return ++(*p);
#endif
}

uint32_t
atomic_dec_32_nv(volatile uint32_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    return __sync_fetch_and_sub(p, 1) - 1;
#elif defined(WIN32)
    return InterlockedDecrement(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    return _InterlockedDecrement((volatile int32_t *)p);
#elif defined(HAVE_PTHREAD)
    uint32_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = --(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return --(*p);
#endif
}

uint64_t
atomic_inc_64_nv(volatile uint64_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    return __sync_fetch_and_add(p, 1) + 1;
#elif defined(WIN32)
    return InterlockedIncrement64(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    return _InterlockedIncrement64((volatile int64_t *)p);
#elif defined(HAVE_PTHREAD)
    uint64_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = ++(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return ++(*p);
#endif
}

uint64_t
atomic_dec_64_nv(volatile uint64_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    return __sync_fetch_and_sub(p, 1) - 1;
#elif defined(WIN32)
    return InterlockedDecrement64(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    return _InterlockedDecrement64((volatile int64_t *)p);
#elif defined(HAVE_PTHREAD)
    uint64_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = --(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return --(*p);
#endif
}

void *
atomic_cas_ptr(volatile void **p, void *oldval, void *newval)
{
#ifdef HAVE___ATOMIC
    volatile void *expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return (void *)/*drop volatile*/expected;
#elif defined(HAVE___SYNC)
    return __sync_val_compare_and_swap((void **)/*drop volatile*/p, oldval, newval);
#elif defined(WIN32)
    return InterlockedCompareExchangePointer(p, newval, oldval);
#elif defined(HAVE_INTEL_INTRINSICS)
    return _InterlockedCompareExchangePointer(p, newval, oldval);
#elif defined(HAVE_PTHREAD)
    volatile void *v;
    (void) pthread_mutex_lock(&atomic_lock);
    if ((v = *p) == oldval)
        *p = newval;
    (void) pthread_mutex_unlock(&atomic_lock);
    return (void *)/*drop volatile*/v;
#else
    volatile void *v = *p;
    if (v == oldval)
        *p = newval;
    return (void *)/*drop volatile*/v;
#endif
}

uint32_t
atomic_cas_32(volatile uint32_t *p, uint32_t oldval, uint32_t newval)
{
#ifdef HAVE___ATOMIC
    uint32_t expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#elif defined(HAVE___SYNC)
    return __sync_val_compare_and_swap(p, oldval, newval);
#elif defined(WIN32)
    return InterlockedCompareExchange32(p, newval, oldval);
#elif defined(HAVE_INTEL_INTRINSICS)
    return _InterlockedCompareExchange(p, newval, oldval);
#elif defined(HAVE_PTHREAD)
    uint32_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    if ((v = *p) == oldval)
        *p = newval;
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    uint32_t v = *p;
    if (v == oldval)
        *p = newval;
    return v;
#endif
}

uint64_t
atomic_cas_64(volatile uint64_t *p, uint64_t oldval, uint64_t newval)
{
#ifdef HAVE___ATOMIC
    uint64_t expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#elif defined(HAVE___SYNC)
    return __sync_val_compare_and_swap(p, oldval, newval);
#elif defined(WIN32)
    return InterlockedCompareExchange64(p, newval, oldval);
#elif defined(HAVE_INTEL_INTRINSICS)
    return _InterlockedCompareExchange64(p, newval, oldval);
#elif defined(HAVE_PTHREAD)
    uint64_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    if ((v = *p) == oldval)
        *p = newval;
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    uint64_t v = *p;
    if (v == oldval)
        *p = newval;
    return v;
#endif
}

void *
atomic_read_ptr(volatile void **p)
{
#ifdef HAVE___ATOMIC
    return __atomic_load_n((void **)/*drop volatile*/p, __ATOMIC_ACQUIRE);
#elif defined(HAVE___SYNC)
    void *junk = NULL;
    return __sync_val_compare_and_swap((void **)/*drop volatile*/p, &junk, &junk);
#elif defined(WIN32)
    void *junk = NULL;
    return InterlockedCompareExchangePointer(p, &junk, &junk);
#elif defined(HAVE_PTHREAD)
    volatile void *v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = *p;
    (void) pthread_mutex_unlock(&atomic_lock);
    return (void *)/*drop volatile*/v;
#else
    return (void *)/*drop volatile*/*p;
#endif
}

uint32_t
atomic_read_32(volatile uint32_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#elif defined(HAVE___SYNC)
    uint32_t junk = 0;
    return __sync_val_compare_and_swap((void **)/*drop volatile*/p, &junk, &junk);
#elif defined(WIN32)
    uint32_t junk = 0;
    return InterlockedCompareExchange32(p, &junk, &junk);
#elif defined(HAVE_PTHREAD)
    uind32_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = *p;
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return *p;
#endif
}

uint64_t
atomic_read_64(volatile uint64_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#elif defined(HAVE___SYNC)
    uint64_t junk = 0;
    return __sync_val_compare_and_swap((void **)/*drop volatile*/p, &junk, &junk);
#elif defined(WIN32)
    uint64_t junk = 0;
    return InterlockedCompareExchange64(p, &junk, &junk);
#elif defined(HAVE_PTHREAD)
    uind64_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = *p;
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return *p;
#endif
}

void
atomic_write_ptr(volatile void **p, void *v)
{
#ifdef HAVE___ATOMIC
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
    return;
#elif defined(HAVE___SYNC) || defined(WIN32)
    void *junk = NULL;
    void *old;
    
    old = atomic_cas_ptr(p, &junk, &junk);
    junk = atomic_cas_ptr(p, old, v);

    /*
     * To be correct we'd have to loop doing the above two CASes, but we
     * assume the writer is releasing p, which they've acquired and is
     * stable.  This way we don't have to loop.
     */
    assert(junk == old);
    return;
#elif defined(HAVE_PTHREAD)
    volatile void *v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = *p;
    (void) pthread_mutex_unlock(&atomic_lock);
    return (void *)/*drop volatile*/v;
#else
    return (void *)/*drop volatile*/*p;
#endif
}

void
atomic_write_32(volatile uint32_t *p, uint32_t v)
{
#ifdef HAVE___ATOMIC
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
    return;
#elif defined(HAVE___SYNC) || defined(WIN32)
    uint32_t junk = NULL;
    uint32_t old;
    
    old = atomic_cas_32(p, &junk, &junk);
    junk = atomic_cas_32(p, old, v);

    /*
     * To be correct we'd have to loop doing the above two CASes, but we
     * assume the writer is releasing p, which they've acquired and is
     * stable.  This way we don't have to loop.
     */
    assert(junk == old);
    return;
#elif defined(HAVE_PTHREAD)
    volatile void *v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = *p;
    (void) pthread_mutex_unlock(&atomic_lock);
    return (void *)/*drop volatile*/v;
#else
    return (void *)/*drop volatile*/*p;
#endif
}

void
atomic_write_64(volatile uint64_t *p, uint64_t v)
{
#ifdef HAVE___ATOMIC
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
    return;
#elif defined(HAVE___SYNC) || defined(WIN32)
    uint64_t junk = NULL;
    uint64_t old;
    
    old = atomic_cas_64(p, &junk, &junk);
    junk = atomic_cas_64(p, old, v);

    /*
     * To be correct we'd have to loop doing the above two CASes, but we
     * assume the writer is releasing p, which they've acquired and is
     * stable.  This way we don't have to loop.
     */
    assert(junk == old);
    return;
#elif defined(HAVE_PTHREAD)
    volatile void *v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = *p;
    (void) pthread_mutex_unlock(&atomic_lock);
    return (void *)/*drop volatile*/v;
#else
    return (void *)/*drop volatile*/*p;
#endif
}
