/*
 * Copyright (c) 2023 Cryptonector LLC
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

/*
 * This implements a generic hazard pointer library.
 */

#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hazards.h"
#include "atomics.h"

#if 0
#define OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define ALIGNOF(type) OFFSETOF(struct { char c; type member; }, member)
#endif

MAKE_ATOMICS_OF_TYPE(hazard, ctp_hazards)

/**
 * Given a pointer to the head of a list of per-thread hazard pointers,
 * a value, and a destructor, call the destructor on that value IFF
 * none of the hazard pointers refer to that value.
 */
void
ctp_hazards_gc(ctp_hazards *hp, void *value, ctp_hazards_dtor_f dtor)
{
    ctp_hazards h;

    for (h = atomic_read_hazard(hp);
         h;
         h = atomic_read_hazard(&h->next)) {
        if (atomic_read_ptr(&h->value) == value)
            return;
    }
    dtor(value);
}

/**
 * Destructor function for a thread-specific key referring to a single
 * per-thread hazard pointer.
 */
void
ctp_hazards_thread_exit(ctp_hazards h)
{
    atomic_write_32(&h->inuse, 0);
}

static int
ptrcmp(const void *va, const void *vb)
{
    void * const *ap = va;
    void * const *bp = vb;

    return (uintptr_t)*ap - (uintptr_t)*bp;
}

/**
 * Reclaim all the hazard pointers on a list of per-thread hazard
 * pointers, destroying the values therein if a non-NULL `dtor' is
 * given.
 */
void
ctp_hazards_destroy(ctp_hazards *hp, ctp_hazards_dtor_f dtor)
{
    ctp_hazards h, hnext;
    ctp_hazards *a;
    size_t i, n = 0;
    void *v = NULL;

    if (dtor == NULL) {
        /* No destructor -> easy case */
        for (h = *hp; h; h = hnext) {
            hnext = h->next;
            free(h);
        }
        *hp = NULL;
        return;
    }

    /* Destructor given -> harder case.  Start by counting the values. */
    for (h = *hp; h; h = h->next) {
        if (h->value)
            n++;
    }

    if (n == 0) {
        /* No non-NULL values -> easy case after all */
        for (h = *hp; h; h = hnext) {
            hnext = h->next;
            free(h);
        }
        *hp = NULL;
        return;
    }
    
    if ((a = calloc(n, sizeof(void *))) == NULL) {
        ctp_hazards hinner;

        /* ENOMEM -> cleanup with O(N^2) uniq step */
        for (h = *hp; h; h = h->next) {
            if (h->value == NULL)
                continue;
            for (hinner = *hp; hinner; hinner = hinner->next) {
                if (hinner != h && hinner->value == h->value)
                    hinner->value = NULL;
            }
        }
        /* Destroy the values */
        for (h = *hp; h; h = hnext) {
            hnext = h->next;
            if (h->value)
                dtor((void *)h->value);
            free(h);
        }
        *hp = NULL;
        return;
    }

    /* Sort uniq the list and free everything */

    /* Collect non-NULL values into `a' then sort */
    for (i = 0, h = *hp; h; h = h->next) {
        if (h->value == NULL)
            continue;
        a[i++] = (void *)h->value;
    }
    qsort(a, n, sizeof(a[0]), ptrcmp);

    /* Destroy unique values in `a' */
    for (v = NULL, i = 0; i < n; i++) {
        if (a[i] == v)
            continue;
        dtor(a[i]);
        v = a[i];
    }
    /* Free the list and `a' */
    for (h = *hp; h; h = hnext) {
        hnext = h->next;
        free(h);
    }
    free(a);
    *hp = NULL;
}

/**
 * Given an abstracted thread-specific key return the calling thread's
 * hazard pointer.
 */
ctp_hazards
ctp_hazards_get(volatile ctp_hazards *hp,
                void *key,
                ctp_key_getspecific_f get,
                ctp_key_setspecific_f set)
{
    ctp_hazards h, first;
    
    if ((h = get(key)))
        return h;

    for (h = first = atomic_read_hazard(hp);
         h;
         h = atomic_read_hazard(&h->next)) {
        if (atomic_cas_32(&h->inuse, 0, 1) == 0) {
            if (set(key, h)) {
                atomic_write_32(&h->inuse, 0);
                return NULL;
            }
            return h;
        }
    }

    if ((h = calloc(1, sizeof(*h))) == NULL)
        return NULL;

    h->inuse = 1;
    h->value = NULL;
    h->next = first;

    if (set(key, h)) {
        free(h);
        return NULL;
    }

    /* Add this hazard to the hazards list */
    while ((first = atomic_cas_hazard(hp, first, h)) != h->next)
        h->next = first;
    return h;
}

void *
ctp_hazards_take(ctp_hazards h, volatile void **vp)
{
    void *newest;

    while (atomic_read_ptr(&h->value) != (newest = atomic_read_ptr(vp)))
        atomic_write_ptr(&h->value, newest);

    return newest;
}

void
ctp_hazards_put(ctp_hazards h)
{
    atomic_write_ptr(&h->value, NULL);
}

