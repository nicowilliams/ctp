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
 * This implements a generic descriptor table.
 */

#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "array_rope.h"
#include "key.h"
#include "desc_tbl.h"
#include "atomics.h"

typedef struct desc_tbl_hazards *desc_tbl_hazards;

/*
 * A descriptor table is an "array" of [wrapped] elements composed of
 * chunks, each larger than the previous chunk by at least .5x.  The
 * table does not shrink, but descriptors are allocated like POSIX file
 * descriptors: producing the lowest available descriptor table index.
 *
 * We'll be using this data structure in the thread-safe variable (TSV)
 * implementation to share one pthread_key_t with all TSVs, and also to
 * implement a named TSV facility.
 *
 * The user gets to use descriptors as a {pointer, verifier} or an
 * {index, verifier}.  The verifier is used to provide some safety.
 */
struct desc_tbl_s {
    ctp_key key;                /* Per-thread hazard pointers, linked up */
    desc_tbl_hazards hazards;   /* All the hazard pointers for this tbl */
    desc_tbl_close_f closef;    /* Descriptor value destructor */
    desc_tbl next;              /* Next chunk */
    desc_tbl_elt elts;          /* This chunk */
    size_t nelts;               /* Allocations in this chunk */
};

/* An element wrapper has a value and a verifier */
struct desc_tbl_elt_s {
    volatile void *value;       /* NULL              -> available */
    volatile uint64_t verifier; /* 0 or (uint64_t)-1 -> not yet open */
};

/* XXX A random number would be a better verifier */
static volatile uint64_t next_verifier = 1;

/* Hazard pointers (one per-dec_tbl) */
struct desc_tbl_hazards {
    volatile desc_tbl tbl;
    volatile void *value;
    struct desc_tbl_hazards *next;
    volatile uint32_t inuse;
};

/* If a table has a `closef' this will call it on `value' if it's safe */
static void
gc(desc_tbl tbl, void *value)
{
    desc_tbl_hazards h;

    for (h = atomic_read_ptr((volatile void **)&tbl->hazards);
         h;
         h = atomic_read_ptr((volatile void **)&h->next)) {
        if (atomic_read_ptr(&h->value) == value)
            return;
    }
    tbl->closef(value);
}

/* Cleanup function for ctp_key */
static void
cleanup(void *value)
{
    struct desc_tbl_hazards *h = value;

    /* This will allow garbage collection of closed descriptor values */
    atomic_write_32(&h->inuse, 0);
}

static int
calloc_chunk(size_t prev_nelts, desc_tbl *out)
{
    size_t nelts = prev_nelts + (prev_nelts >> 1) + 8;
    size_t sz = 2 * sizeof(struct desc_tbl_s) +
        nelts * sizeof(struct desc_tbl_elt_s);

    *out = NULL;

    /* Overflow protection */
    if (nelts >= (SIZE_MAX >> 6) / sizeof(struct desc_tbl_elt_s) ||
        nelts >= (SIZE_MAX >> 6) / (2 * sizeof(struct desc_tbl_s)))
        return EOVERFLOW;

    if ((*out = calloc(1, sz)) == NULL)
        return ENOMEM;
    (*out)->nelts = nelts;
    return 0;
}

/**
 * Allocates and initializes a generic descriptor table.
 *
 * The `closef' callback function may be NULL.  If it is not NULL then a
 * ctp_key may be initialized as part of the descriptor table's
 * internals.
 */
int
desc_tbl_init(volatile desc_tbl *out, desc_tbl_close_f closef)
{
    desc_tbl t;
    size_t i;
    int ret;

    *out = NULL;

    if ((ret = calloc_chunk(0, &t)))
        return ret;
    t->elts = (void *)(t + 1);
    t->closef = closef;
    t->hazards = NULL;

    if (closef) {
        int ret;

        /*
         * We need a ctp_key in order to drive garbage collection when a
         * destructor is provided.  But ctp_key's implementation needs a
         * desc_tbl, so to break the circular dependency desc_tbl will
         * not use a ctp_key when a destructor is not provided, and
         * ctp_key will not provide a destructor.
         */
        if ((ret = ctp_key_create(&t->key, cleanup))) {
            free(t);
            return ret;
        }
    }

    for (i = 0; i < t->nelts; i++)
        t->elts[i].value = NULL;
    atomic_write_ptr((volatile void **)out, t);
    return 0;
}

/**
 * Must be called only when no thread will use `tbl' again, as this
 * function does no synchronization of any kind.
 */
void
desc_tbl_destroy(desc_tbl tbl)
{
    desc_tbl_hazards h, hnext;
    desc_tbl p, next;
    size_t i;

    for (h = tbl->hazards; h; h = hnext) {
        hnext = h->next;
        free(h);
    }
    tbl->hazards = NULL;

    for (next = tbl; next; ) {
        if (tbl->closef) {
            for (i = 0; i < next->nelts; i++)
                if (next->elts[i].value)
                    tbl->closef(next->elts[i].value);
        }
        p = next;
        next = next->next;
        p->next = NULL;
        free(p);
    }
}

/* Return this thread's hazard pointer, allocating if need be */
static desc_tbl_hazards
get_hazard(desc_tbl tbl)
{
    desc_tbl_hazards h, first;
    
    if ((h = ctp_key_getspecific(tbl->key)))
        return h;

    for (h = first = atomic_read_ptr((volatile void **)&tbl->hazards);
         h;
         h = atomic_read_ptr((volatile void **)&h->next)) {
        if (atomic_cas_32(&h->inuse, 0, 1) == 0) {
            if (ctp_key_setspecific(tbl->key, h)) {
                atomic_write_32(&h->inuse, 0);
                return NULL;
            }
            return h;
        }
    }

    if ((h = calloc(1, sizeof(*h))) == NULL)
        return NULL;

    h->tbl = tbl;
    h->inuse = 1;
    h->value = NULL;
    h->next = first;

    if (ctp_key_setspecific(tbl->key, h)) {
        free(h);
        return NULL;
    }

    /* Add this hazard to this desc_tbl's hazards list */
    while ((first = atomic_cas_ptr((volatile void **)&tbl->hazards, first, h)) != h->next)
        h->next = first;

    return h;
}

/**
 * "Opens" (allocates) a descriptor in `tbl', saves `value' as that
 * descriptor's value, and outputs a pointer handle and an integer
 * handle to the descriptor.
 *
 * This tries to allocate the lowest available integer handle, just as
 * with POSIX file descriptors.
 *
 * The `verifier' number output must be used to close descriptors.
 */
int
desc_tbl_open(desc_tbl tbl, void *value, desc_tbl_elt *e, int *n, uint64_t *verifier)
{
    desc_tbl_hazards h = NULL;
    desc_tbl last = tbl;
    desc_tbl p, next;
    size_t i;
    int d = 0;
    int ret;

    *verifier = 0;
    if (!e && !n)
        return EINVAL;
    if (e)
        *e = NULL;
    if (n)
        *n = -1;

    if (tbl->key) {
        if ((h = get_hazard(tbl)) == NULL)
            return ENOMEM;
        atomic_write_ptr((volatile void **)&h->value, value);
    }

    for (;;) {
        /* Look for a free slot */
        for (p = last;
             p && d < (INT_MAX >> 4);
             p = atomic_read_ptr((volatile void **)&p->next)) {
            last = p;
            for (i = 0; i < p->nelts; d++, i++) {
                if (atomic_read_ptr(&p->elts[i].value) == NULL) {
                    if (atomic_cas_ptr(&p->elts[i].value,
                                       NULL, value) == NULL)
                        break; /* Got it! */
                }
            }
            if (i < p->nelts) {
                atomic_write_64(&p->elts[i].verifier, *verifier = atomic_inc_64_nv(&next_verifier));
                if (e)
                    *e = &p->elts[i];
                if (n)
                    *n = d;
                return 0;
            }
        }

        /* Ran off the end; add a new chunk */
        if ((ret = calloc_chunk(last->nelts, &next)))
            return ret;
        next->elts = (void *)(next + 1);
        if (atomic_cas_ptr((volatile void **)&last->next,
                           NULL, next) != NULL)
            free(next); /* Lost the race to add the new chunk */

        /* There's a new chunk now */
    }

    if (h)
        atomic_write_ptr((volatile void **)&h->value, NULL);
    return EMFILE;
}

/**
 * Given a descriptor table element, output its value.
 *
 * If `verifier' does not match the element's verifier number, then
 * `EBADF' will be returned.
 */
int
desc_tbl_get_p(desc_tbl tbl, desc_tbl_elt e, uint64_t verifier, void **vp)
{
    desc_tbl_hazards h;
    void *value;

    *vp = NULL;

    if (atomic_read_64(&e->verifier) != verifier)
        return EBADF;

    value = atomic_read_ptr(&e->value);

    if (tbl->key) {
        if ((h = get_hazard(tbl)) == NULL)
            return ENOMEM;
        atomic_write_ptr((volatile void **)&h->value, value);
    }

    *vp = value;
    return 0;
}

/**
 * Look up descriptor `n' in `tbl' and output either or both of its
 * value and its handle.
 *
 * If `verifier' does not match the element's verifier number, then
 * `EBADF' will be returned.
 */
int
desc_tbl_get_n(desc_tbl tbl, int n, uint64_t verifier, desc_tbl_elt *ep, void **vp)
{
    desc_tbl_hazards h = NULL;
    desc_tbl p;
    size_t d, i;

    if (tbl->key) {
        if ((h = get_hazard(tbl)) == NULL)
            return ENOMEM;
    }

    if (n < 0 || n >= INT_MAX >> 4)
        return EINVAL;

    if (ep)
        *ep = NULL;
    if (vp)
        *vp = NULL;
    for (d = n, p = tbl; p && d < (INT_MAX >> 4); p = p->next) {
        for (i = 0; i <= d && i < p->nelts; d++, i++) {
            if (i == d) {
                if (!verifier || p->elts[i].verifier != verifier)
                    return EBADF;
                if (ep)
                    *ep = &p->elts[i];
                if (vp)
                    *vp = (void *)p->elts[i].value;
                if (h)
                    atomic_write_ptr((volatile void **)&h->value,
                                     (void *)p->elts[i].value);
                return 0;
            }
        }
        if (i > d)
            return ENOENT;
        d -= p->nelts;
    }
    return ENOENT;
}

/**
 * Release the reference to the last opened/fetched descriptor value.
 */
void
desc_tbl_put(desc_tbl tbl)
{
    desc_tbl_hazards h;

    if (tbl->key && (h = get_hazard(tbl))) {
        void *v = atomic_read_ptr((volatile void **)&h->value);

        atomic_write_ptr((volatile void **)&h->value, NULL);
        gc(tbl, v);
    }
}

struct desc_tbl_cursor_s {
    desc_tbl tbl;
    size_t i;
    int d;
};

void
desc_tbl_iter_free(desc_tbl_cursor *cursorp)
{
    free(*cursorp);
    *cursorp = NULL;
}

/**
 * Iterate through all the descriptors.
 *
 * Call this with `*cursorp = NULL' to start the iteration.  Returns -1
 * to indicate that the end of the descriptor table has been reached.
 *
 * Calling `desc_tbl_get_n()' over all possible descriptors is not as
 * performant as using this iterator.
 */
int
desc_tbl_iter(desc_tbl tbl,
              desc_tbl_cursor *cursorp,
              int *dp,
              uint64_t *verifierp,
              desc_tbl_elt *ep,
              void **vp)
{
    desc_tbl_cursor cursor = *cursorp;
    uint64_t verifier;
    desc_tbl_elt e;
    int d = -1;

    /* Initialize outputs */
    if (dp)
        *dp = -1;
    if (verifierp)
        *verifierp = 0;
    if (ep)
        *ep = NULL;
    if (vp)
        *vp = NULL;
    if (cursor == NULL) {
        if ((cursor = calloc(1, sizeof(*cursor))) == NULL)
            return ENOMEM;
        cursor->tbl = tbl;
        cursor->i = 0;
        cursor->d = 0;
        *cursorp = cursor;
    }

    do {
        if (cursor->tbl && cursor->i == cursor->tbl->nelts) {
            cursor->tbl = atomic_read_ptr((volatile void **)&cursor->tbl->next);
            cursor->i = 0;
        }

        if (cursor->tbl == NULL) {
            free(cursor);
            *cursorp = NULL;
            return -1;
        }

        e = &cursor->tbl->elts[cursor->i++];
        d = cursor->d++;
    } while (atomic_read_ptr(&e->value) == NULL ||
             (verifier = atomic_read_64(&e->verifier) == 0) ||
             verifier == (uint64_t)-1);

    if (dp)
        *dp = d;
    if (verifierp)
        *verifierp = e->verifier;
    if (ep)
        *ep = e;
    if (vp)
        *vp = (void *)e->value;
    return 0;
}

/**
 * Close a descriptor identified by its handle.
 *
 * The `verifier' must not be zero, and it must match the descriptor's.
 *
 * The caller must be certain that there are no dangling references to
 * the descriptor and its verifier.
 */
int
desc_tbl_close_p(desc_tbl tbl, desc_tbl_elt e, uint64_t verifier)
{
    void *v = atomic_read_ptr(&e->value);

    if (verifier == 0 || atomic_read_64(&e->verifier) != verifier)
        return EBADF;
    atomic_write_64(&e->verifier, (uint64_t)-1);
    atomic_write_ptr(&e->value, NULL);
    if (v && tbl->closef)
        gc(tbl, v);
    return 0;
}

/**
 * Close a descriptor identified by its number.
 *
 * The `verifier' must not be zero, and it must match the descriptor's.
 *
 * The caller must be certain that there are no dangling references to
 * the descriptor and its verifier.
 */
int
desc_tbl_close_n(desc_tbl tbl, int d, uint64_t verifier)
{
    desc_tbl_elt e;
    void *v;
    int ret;

    if (verifier == 0)
        return EBADF;
    if ((ret = desc_tbl_get_n(tbl, d, verifier, &e, &v)))
        return ret;
    atomic_write_64(&e->verifier, (uint64_t)-1);
    atomic_write_ptr(&e->value, NULL);
    /*
     * XXX We could really use a TSV here so we don't destroy live
     * values, but TSV wants to use this facility, so we'd have a
     * circularity.  What to do?
     *
     * Answer: add a reference count to elements?  No, because we'd
     *         still have to deal with the need to compose multiple
     *         atomic operations.  Ay!
     */
    if (v && tbl->closef)
        return tbl->closef(v);
    return 0;
}
