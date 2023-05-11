/*
 * Copyright (c) 2015 Cryptonector LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

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

#ifdef USE_HELGRIND
#if defined(HAVE___ATOMIC) || defined(HAVE___SYNC) || defined(WIN32) || defined(HAVE_INTEL_INTRINSICS)
#include <valgrind/helgrind.h>
#else
#define ANNOTATE_HAPPENS_BEFORE(v)
#define ANNOTATE_HAPPENS_AFTER(v)
#endif
#else
#define ANNOTATE_HAPPENS_BEFORE(v)
#define ANNOTATE_HAPPENS_AFTER(v)
#endif


uint32_t
atomic_inc_32_nv(volatile uint32_t *p)
{
    uint32_t r;

    ANNOTATE_HAPPENS_AFTER(*p);

#ifdef HAVE___ATOMIC
    r = __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    r = __sync_fetch_and_add(p, 1) + 1;
#elif defined(WIN32)
    r = InterlockedIncrement(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    r = _InterlockedIncrement((volatile int32_t *)p);
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    r = ++(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    r = ++(*p);
#endif

    ANNOTATE_HAPPENS_BEFORE(*p);
    return r;
}

uint32_t
atomic_dec_32_nv(volatile uint32_t *p)
{
    uint32_t r;

    ANNOTATE_HAPPENS_AFTER(*p);

#ifdef HAVE___ATOMIC
    r = __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    r = __sync_fetch_and_sub(p, 1) - 1;
#elif defined(WIN32)
    r = InterlockedDecrement(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    r = _InterlockedDecrement((volatile int32_t *)p);
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    r = --(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    r = --(*p);
#endif

    ANNOTATE_HAPPENS_BEFORE(*p);
    return r;
}

uint64_t
atomic_inc_64_nv(volatile uint64_t *p)
{
    uint64_t r;

    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    r = __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    r = __sync_fetch_and_add(p, 1) + 1;
#elif defined(WIN32)
    r = InterlockedIncrement64(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    r = _InterlockedIncrement64((volatile int64_t *)p);
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    r = ++(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    r = ++(*p);
#endif

    ANNOTATE_HAPPENS_BEFORE(*p);
    return r;
}

uint64_t
atomic_dec_64_nv(volatile uint64_t *p)
{
    uint64_t r;

    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    r = __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
#elif defined(HAVE___SYNC)
    r = __sync_fetch_and_sub(p, 1) - 1;
#elif defined(WIN32)
    r = InterlockedDecrement64(p);
#elif defined(HAVE_INTEL_INTRINSICS)
    r = _InterlockedDecrement64((volatile int64_t *)p);
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    r = --(*p);
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    r = --(*p);
#endif

    ANNOTATE_HAPPENS_BEFORE(*p);
    return r;
}

void *
atomic_cas_ptr(volatile void **p, const void *oldval, const void *newval)
{
    void *r;

    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    volatile const void *expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    r = (void *)(uintptr_t)/*drop volatile*/expected;
#elif defined(HAVE___SYNC)
    r = (void *)(uintptr_t)__sync_val_compare_and_swap(p, oldval, newval);
#elif defined(WIN32)
    r = InterlockedCompareExchangePointer(p, newval, oldval);
#elif defined(HAVE_INTEL_INTRINSICS)
    r = _InterlockedCompareExchangePointer(p, newval, oldval);
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    if ((r = *p) == oldval)
        *p = newval;
    (void) pthread_mutex_unlock(&atomic_lock);
    r = (void *)/*drop volatile*/v;
#else
    r = *p;
    if (v == oldval)
        *p = newval;
    r = (void *)/*drop volatile*/v;
#endif

    ANNOTATE_HAPPENS_BEFORE(*p);
    return r;
}

uint32_t
atomic_cas_32(volatile uint32_t *p, uint32_t oldval, uint32_t newval)
{
    uint32_t r;

    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    uint32_t expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    r = expected;
#elif defined(HAVE___SYNC)
    r = __sync_val_compare_and_swap(p, oldval, newval);
#elif defined(WIN32)
    r = InterlockedCompareExchange32(p, newval, oldval);
#elif defined(HAVE_INTEL_INTRINSICS)
    r = _InterlockedCompareExchange(p, newval, oldval);
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    if ((r = *p) == oldval)
        *p = newval;
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    r = *p;
    if (v == oldval)
        *p = newval;
#endif

    ANNOTATE_HAPPENS_BEFORE(*p);
    return r;
}

uint64_t
atomic_cas_64(volatile uint64_t *p, uint64_t oldval, uint64_t newval)
{
    uint64_t r;

    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    uint64_t expected = oldval;
    (void) __atomic_compare_exchange_n(p, &expected, newval, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    r = expected;
#elif defined(HAVE___SYNC)
    r = __sync_val_compare_and_swap(p, oldval, newval);
#elif defined(WIN32)
    r = InterlockedCompareExchange64(p, newval, oldval);
#elif defined(HAVE_INTEL_INTRINSICS)
    r = _InterlockedCompareExchange64(p, newval, oldval);
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    if ((r = *p) == oldval)
        *p = newval;
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    r = *p;
    if (v == oldval)
        *p = newval;
#endif

    ANNOTATE_HAPPENS_BEFORE(*p);
    return r;
}

void *
atomic_read_ptr(volatile void **p)
{
    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    return __atomic_load_n((void **)(uintptr_t)/*drop volatile*/p, __ATOMIC_ACQUIRE);
#elif defined(HAVE___SYNC)
    void *junk = 0;
    return (void **)(uintptr_t)__sync_val_compare_and_swap(p, &junk, &junk);
#elif defined(WIN32)
    void *junk = 0;
    return InterlockedCompareExchangePointer(p, &junk, &junk);
#elif defined(HAVE_PTHREAD)
    {
        void *v;
        (void) pthread_mutex_lock(&atomic_lock);
        v = (void *)p;
        (void) pthread_mutex_unlock(&atomic_lock);
        return v;
    }
#else
    return (void *)/*drop volatile*/*p;
#endif
}

uint32_t
atomic_read_32(volatile uint32_t *p)
{
    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#elif defined(HAVE___SYNC)
    uint32_t junk = 0;
    return __sync_val_compare_and_swap(p, &junk, &junk);
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
    ANNOTATE_HAPPENS_AFTER(*p);
#ifdef HAVE___ATOMIC
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#elif defined(HAVE___SYNC)
    uint64_t junk = 0;
    return __sync_val_compare_and_swap(p, &junk, &junk);
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
atomic_write_ptr(volatile void **p, const void *v)
{
#ifdef HAVE___ATOMIC
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
#elif defined(HAVE___SYNC) || defined(WIN32)
    {
        void *junk = 0;
        void *old;

        old = atomic_cas_ptr(p, &junk, &junk);
        junk = atomic_cas_ptr(p, old, v);

        /*
         * To be correct we'd have to loop doing the above two CASes, but we
         * assume the writer is releasing p, which they've acquired and is
         * stable.  This way we don't have to loop.
         */
    }
#elif defined(HAVE_PTHREAD)
    {
        (void) pthread_mutex_lock(&atomic_lock);
        *p = v;
        (void) pthread_mutex_unlock(&atomic_lock);
    }
#else
    *p = v;
#endif
    ANNOTATE_HAPPENS_BEFORE(*p);
}

void
atomic_write_32(volatile uint32_t *p, uint32_t v)
{
#ifdef HAVE___ATOMIC
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
#elif defined(HAVE___SYNC) || defined(WIN32)
    {
        uint32_t junk = 0;
        uint32_t old;

        old = atomic_cas_32(p, junk, junk);
        junk = atomic_cas_32(p, old, v);

        /*
         * To be correct we'd have to loop doing the above two CASes, but we
         * assume the writer is releasing p, which they've acquired and is
         * stable.  This way we don't have to loop.
         */
    }
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    *p = v;
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    *p = v;
#endif
    ANNOTATE_HAPPENS_BEFORE(*p);
}

void
atomic_write_64(volatile uint64_t *p, uint64_t v)
{
#ifdef HAVE___ATOMIC
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
#elif defined(HAVE___SYNC) || defined(WIN32)
    {
        uint64_t junk = 0;
        uint64_t old;

        /* A way to force a memory fence? */
        old = atomic_cas_64(p, junk, junk);
        junk = atomic_cas_64(p, old, v);

        /*
         * To be correct we'd have to loop doing the above two CASes, but we
         * assume the writer is releasing p, which they've acquired and is
         * stable.  This way we don't have to loop.
         */
    }
#elif defined(HAVE_PTHREAD)
    (void) pthread_mutex_lock(&atomic_lock);
    *p = v;
    (void) pthread_mutex_unlock(&atomic_lock);
#else
    *p = v;
#endif
    ANNOTATE_HAPPENS_BEFORE(*p);
}
