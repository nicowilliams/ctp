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

#define OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define ALIGNOF(type) OFFSETOF(struct { char c; type member; }, member)

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
    array_rope a;               /* The descriptors array */
};

/* An element wrapper has a value and a verifier */
struct desc_tbl_elt_s {
    volatile void *value;       /* NULL              -> available */
    volatile uint64_t verifier; /* 0 or (uint64_t)-1 -> not yet open */
};

/* XXX A random number would be a better verifier */
static volatile uint64_t next_verifier = 1;

/* Hazard pointers (one per-desc_tbl) */
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
    int ret;

    *out = NULL;

    if ((t = calloc(1, sizeof(*t))) == NULL)
        return ENOMEM;
    if ((ret = array_rope_init(&t->a, ALIGNOF(struct desc_tbl_elt_s)))) {
        free(t);
        return ret;
    }
    t->closef = closef;
    t->hazards = NULL;

    if (closef) {
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

    atomic_write_ptr((volatile void **)out, t);
    return 0;
}

/**
 * Must be called only when no thread will use `tbl' again, as this
 * function does no synchronization of any kind.
 */
void
desc_tbl_destroy(desc_tbl *tblp)
{
    array_rope_cursor c = NULL;
    desc_tbl_hazards h, hnext;
    desc_tbl tbl = *tblp;

    *tblp = NULL;

    for (h = tbl->hazards; h; h = hnext) {
        hnext = h->next;
        free(h);
    }
    tbl->hazards = NULL;

    if (tbl->closef) {
        array_rope_cursor c = NULL;
        void *ve;
        int ret, idx;

        while ((ret = array_rope_iter(tbl->a, &c, &idx, &ve)) == 0) {
            desc_tbl_elt e = ve;

            if (e->value)
                tbl->closef(e->value);
        }
    }
    array_rope_iter_free(&c);
    array_rope_destroy(&tbl->a);
    free(tbl);
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
desc_tbl_open(desc_tbl tbl, void *value, desc_tbl_elt *ep, int *np, uint64_t *verifier)
{
    array_rope_cursor c = NULL;
    desc_tbl_hazards h = NULL;
    void *ve;
    int ret;

    *verifier = atomic_inc_64_nv(&next_verifier);
    if (!ep && !np)
        return EINVAL;
    if (ep)
        *ep = NULL;
    if (np)
        *np = -1;

    if (tbl->key) {
        if ((h = get_hazard(tbl)) == NULL)
            return ENOMEM;
        atomic_write_ptr(&h->value, NULL);
    }

    /* Look for a free slot */
    while ((ret = array_rope_iter(tbl->a, &c, np, &ve)) == 0) {
        desc_tbl_elt e = ve;

        if (atomic_cas_ptr(&e->value, NULL, value) == NULL &&
            atomic_cas_64(&e->verifier, 0, *verifier) == 0) {
            array_rope_iter_free(&c);
            atomic_write_ptr(&h->value, value);
            return 0;
        }
    }

    array_rope_iter_free(&c);
    if (ret == -1 && (ret = array_rope_add(tbl->a, &ve, np)) == 0) {
        desc_tbl_elt e = ve;

        atomic_write_ptr(&e->value, value);
        atomic_write_64(&e->verifier, *verifier);
        if (h)
            atomic_write_ptr(&h->value, value);
        return 0;
    }
    return ret;
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
        atomic_write_ptr(&h->value, value);
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
    desc_tbl_elt e;
    void *ve, *value;
    int ret;

    if (ep)
        *ep = NULL;
    if (vp)
        *vp = NULL;

    if (tbl->key) {
        if ((h = get_hazard(tbl)) == NULL)
            return ENOMEM;
    }

    if ((ret = array_rope_get(tbl->a, AR_GO_IF_SET, n, &ve)))
        return ret;
    e = ve;
    if (!verifier || atomic_read_64(&e->verifier) != verifier)
        return EBADF;
    if (ep)
        *ep = ve;
    if (vp) {
        vp = value = atomic_read_ptr(&e->value);
        if (h)
            atomic_write_ptr(&h->value, value);
    }
    return 0;
}

/**
 * Release the reference to the last opened/fetched descriptor value.
 */
void
desc_tbl_put(desc_tbl tbl)
{
    desc_tbl_hazards h;

    if (tbl->key && (h = get_hazard(tbl))) {
        void *v = atomic_read_ptr(&h->value);

        atomic_write_ptr(&h->value, NULL);
        gc(tbl, v);
    }
}

struct desc_tbl_cursor_s {
    array_rope_cursor c;
    desc_tbl tbl;
    size_t i;
    int d;
};

void
desc_tbl_iter_free(desc_tbl_cursor *cursorp)
{
    desc_tbl_cursor c = *cursorp;

    *cursorp = NULL;
    if (c) {
        array_rope_iter_free(&c->c);
        free(c);
    }
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
    desc_tbl_cursor c = *cursorp;
    uint64_t verifier;
    void *ve;
    int ret;

    /* Initialize outputs */
    *dp = -1;
    *verifierp = 0;
    if (ep)
        *ep = NULL;
    *vp = NULL;
    if (c == NULL) {
        if ((c = calloc(1, sizeof(*c))) == NULL)
            return ENOMEM;
        c->tbl = tbl;
        c->c = NULL;
        c->i = 0;
        c->d = 0;
        *cursorp = c;
    }

    while ((ret = array_rope_iter(tbl->a, &c->c, dp, &ve)) == 0) {
        desc_tbl_elt e = ve;

        if (ep)
            *ep = ve;
        *vp = atomic_read_ptr(&e->value);
        *verifierp = verifier = atomic_read_64(&e->verifier);
        
        if (*vp != NULL && verifier != 0 && verifier != (uint64_t)-1)
            return 0;
    }

    array_rope_iter_free(&c->c);
    free(c);
    *cursorp = NULL;
    return ret;
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
