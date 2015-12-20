
#include "atomics.h"

#ifndef HAVE_ILLUMOS_ATOMICS

#ifdef HAVE___ATOMIC
/* Nothing to do */
#elif HAVE___SYNC
/* Nothing to do */
#elif WIN32
#include <windows.h>
#include <WinBase.h>
#elif HAVE_PTHREAD
#include <pthread.h>
pthread_mutex_t atomic_lock = PTHREAD_MUTEX_INITIALIZER;
#elif NO_THREADS
/* Nothing to do */
#else
#error "Error: no acceptable implementation of atomic primitives is available"
#endif

uint32_t
atomic_inc_32_nv(volatile uint32_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif HAVE___SYNC
    return __sync_fetch_and_add(p, 1) + 1;
#elif WIN32
    return InterlockedIncrement64(p);
#elif HAVE_PTHREAD
    uint32_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = ++(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return ++(*p);
#endif
}

uint32_t atomic_dec_32_nv(volatile uint32_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif HAVE___SYNC
    return __sync_fetch_and_sub(p, 1) - 1;
#elif WIN32
    return InterlockedDecrement(p);
#elif HAVE_PTHREAD
    uint32_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = --(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return --(*p);
#endif
}

uint64_t atomic_inc_64_nv(volatile uint64_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif HAVE___SYNC
    return __sync_fetch_and_add(p, 1) + 1;
#elif WIN32
    return InterlockedIncrement64(p);
#elif HAVE_PTHREAD
    uint64_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = ++(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return ++(*p);
#endif
}

uint64_t atomic_dec_64_nv(volatile uint64_t *p)
{
#ifdef HAVE___ATOMIC
    return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif HAVE___SYNC
    return __sync_fetch_and_sub(p, 1) - 1;
#elif WIN32
    return InterlockedDecrement(p);
#elif HAVE_PTHREAD
    uint64_t v;
    (void) pthread_mutex_lock(&atomic_lock);
    v = --(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
    return v;
#else
    return --(*p);
#endif
}

void *atomic_cas_ptr(volatile void **p, void *oldval, void *newval)
{
#ifdef HAVE___ATOMIC
    volatile void *expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return (void *)/*drop volatile*/expected;
#elif HAVE___SYNC
    return __sync_val_compare_and_swap((void **)/*drop volatile*/p, oldval, newval);
#elif WIN32
    return InterlockedCompareExchangePointer(p, newval, oldval);
#elif HAVE_PTHREAD
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

uint32_t atomic_cas_32(volatile uint32_t *p, uint32_t oldval, uint32_t newval)
{
#ifdef HAVE___ATOMIC
    uint32_t expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#elif HAVE___SYNC
    return __sync_val_compare_and_swap(p, oldval, newval);
#elif WIN32
    return InterlockedCompareExchange32(p, newval, oldval);
#elif HAVE_PTHREAD
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

uint64_t atomic_cas_64(volatile uint64_t *p, uint64_t oldval, uint64_t newval)
{
#ifdef HAVE___ATOMIC
    uint64_t expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
#elif HAVE___SYNC
    return __sync_val_compare_and_swap(p, oldval, newval);
#elif WIN32
    return InterlockedCompareExchange64(p, newval, oldval);
#elif HAVE_PTHREAD
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
#endif
