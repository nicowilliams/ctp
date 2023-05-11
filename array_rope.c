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
 * This implements a generic growing array.
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
#include "atomics.h"

/*
 * An array rope is an "array" of smaller chunks, each larger than the
 * previous chunk by at least .5x.  The array rope does not shrink, only
 * grows.
 */
struct array_rope_s {
    array_rope next;            /* Next chunk */
    void *elts;                 /* This chunk's elements */
    uint32_t elt_align_size;    /* Alignment size of elements */
    uint32_t nalloced;          /* Number of elements alloced in this chunk */
    uint32_t nelts;             /* Number of elements used    in this chunk */
};

/**
 * Allocates and initializes an array rope of elements whose alignment
 * size is `elt_align_size'.
 */
int
array_rope_init(volatile array_rope *out, size_t elt_align_size)
{
    array_rope t;

    *out = NULL;
    if (elt_align_size > UINT16_MAX)
        return EOVERFLOW;
    /* Overflow check not needed here because 8 * UINT16_MAX < UINT32_MAX */
    if ((t = calloc(1, sizeof(*t) * 2 + 8 * elt_align_size)) == NULL)
        return ENOMEM;
    t->elt_align_size = elt_align_size;
    t->nalloced = 8;
    t->nelts = 0;
    t->elts = (void *)(t + 1);

    atomic_write_ptr((volatile void **)out, t);
    return 0;
}

/**
 * Frees an array rope.
 *
 * Must be called only when no thread will use `*ap' again, as this
 * function does no synchronization of any kind.  The caller must do any
 * actions needed to release the contents of each array element (but
 * must not free the element itself).
 */
void
array_rope_destroy(array_rope *ap)
{
    array_rope a = *ap;
    array_rope p, next;

    for (next = a; next; ) {
        p = next;
        next = next->next;
        p->next = NULL;
        free(p);
    }
    atomic_write_ptr((volatile void **)ap, NULL);
}

static int
grow(array_rope last)
{
    array_rope next;

    if (SIZE_MAX / last->elt_align_size <=
        (last->nalloced + (last->nalloced >> 1) + 4) + sizeof(*next))
        return EOVERFLOW;

    if ((next = calloc(1,
                       sizeof(*next) * 2 +
                       (last->nalloced + (last->nalloced >> 1) + 4) *
                       last->elt_align_size)) == NULL)
        return ENOMEM;

    /* Atomics not needed for reads from `last' now */
    next->next = NULL;
    next->elt_align_size = last->elt_align_size;
    next->nalloced = last->nalloced + (last->nalloced >> 1) + 4;
    next->nelts = 0;
    next->elts = (void *)(next + 1);

    /* Add the new chunk */
    if (atomic_cas_ptr((volatile void **)&last->next, NULL, next) != NULL)
        free(next); /* Lost the race to add the new chunk */
    return 0;
}

/**
 * Allocates and appends an element to the array rope.
 */
int
array_rope_add(array_rope a, void **ep, int *ip)
{
    array_rope last = a;
    array_rope p;
    size_t i;
    int idx = 0;
    int ret;

    *ep = NULL;
    *ip = -1;
    while (idx < (INT_MAX >> 4)) {
        /* Look for a free slot */
        for (p = last;
             p && idx < (INT_MAX >> 4);
             p = atomic_read_ptr((volatile void **)&p->next)) {
            last = p;
            for (i = atomic_read_32(&p->nelts);
                 i < atomic_read_32(&p->nalloced); /* XXX Atomic not needed here... */
                 i = atomic_read_32(&p->nelts)) {
                if (atomic_cas_32(&p->nelts, i, i + 1) == i) {
                    *ep = i * p->elt_align_size + (unsigned char *)p->elts;
                    *ip = i + idx;
                    return 0;
                }
            }
            idx += p->nalloced; /* No atomic needed here */
        }

        /* Ran off the end; grow it */
        if ((ret = grow(last)))
            return ret;
        /* There's a new chunk now, so try again */
        last = atomic_read_ptr((volatile void **)&last->next);
    }
    return EMFILE;
}

/**
 * Get the pointer to the `idx''th element of `a'.
 */
int
array_rope_get(array_rope a, array_rope_get_options o, int idx, void **ep)
{
    array_rope p, last;
    uint32_t d;

    *ep = NULL;
    if (idx < 0 || idx >= INT_MAX >> 4)
        return EINVAL;

    for (d = 0, p = a;
         p;
         d += atomic_read_32(&p->nalloced), p = p->next) {
        uint32_t i, nelts;

        last = p;

        i = (uint32_t)idx - d;

        if (i > atomic_read_32(&p->nalloced)) {
            /*
             * Need to go to the next chunk.  If there's no next chunk
             * we'll fall out of the loop; see commentary there.
             */
            if ((nelts = atomic_read_32(&p->nelts)) != p->nalloced) {
                if (o != AR_GO_FORCE)
                    return ENOENT;
                atomic_write_32(&p->nelts, p->nalloced);
            }
            continue;
        }

        /* The index we're looking for is in this chunk */
        if ((nelts = atomic_read_32(&p->nelts)) < i + 1) {
            if (o != AR_GO_FORCE)
                return ENOENT;
            /* Logically grow this chunk */
            while ((nelts = atomic_cas_32(&p->nelts, nelts, i + 1)) < i + 1)
                ;
        }

        *ep = i * atomic_read_32(&p->elt_align_size) + (unsigned char *)p->elts;
        return 0;
    }
    if (o == AR_GO_FORCE) {
        grow(last);
        /* Tail-call recurse to retry now that we've grown the array */
        return array_rope_get(atomic_read_ptr((volatile void **)&last->next), o, idx - d, ep);
    }
    return ENOENT;
}

void *
array_rope_getp(array_rope a, array_rope_get_options o, int idx)
{
    void *v;
    int ret;

    if ((ret = array_rope_get(a, o, idx, &v)))
        errno = ret;
    return v;
}

/**
 * Given an element pointer previously produced by `array_rope_add()' or
 * `array_rope_get()', return its index.
 *
 * Returns -1 if the given pointer `data' is not a pointer to an element
 * in the given array rope `a'.
 */
int
array_rope_get_index(array_rope a, void *data)
{
    uintptr_t e = (uintptr_t)data;
    uint32_t d;

    if (e == 0)
        return -1;

    for (d = 0; a; d += atomic_read_32(&a->nalloced), a = a->next) {
        uintptr_t astart = (uintptr_t)atomic_read_ptr((volatile void **)&a->elts);
        uintptr_t aend = astart + (uintptr_t)atomic_read_32(&a->nalloced) * a->elt_align_size;

        if (astart <= e && e < aend) {
            uintptr_t n = e - astart;

            if (n % a->elt_align_size != 0)
                return -1;
            n /= a->elt_align_size;
            if (d + n >= INT_MAX)
                return -1;
            return d + n;
        }
    }
    return -1;
}

struct array_rope_cursor_s {
    array_rope a;
    size_t i;
    int d;
};

/**
 * Iterate through all the elements of `a'.
 *
 * Call this with `*cursorp = NULL' to start the iteration.  Returns -1
 * to indicate that the end of the descriptor table has been reached.
 *
 * Calling `array_rope_get()' over all possible indexes is not as
 * performant as using this iterator.
 */
int
array_rope_iter(array_rope a,
                array_rope_cursor *cursorp,
                int *idxp,
                void **ep)
{
    array_rope_cursor cursor = *cursorp;

    /* Initialize outputs */
    *idxp = -1;
    *ep = NULL;

    if (cursor == NULL) {
        if ((cursor = calloc(1, sizeof(*cursor))) == NULL)
            return ENOMEM;
        cursor->a = a;
        cursor->i = 0;
        cursor->d = 0;
        *cursorp = cursor;
    }

    if (cursor->a && cursor->i == atomic_read_32(&cursor->a->nelts)) {
        if (cursor->i < atomic_read_32(&cursor->a->nalloced)) {
            array_rope_iter_free(cursorp);
            return 0;
        }
        cursor->a = atomic_read_ptr((volatile void **)&cursor->a->next);
        cursor->i = 0;
    }

    if (cursor->a == NULL) {
        array_rope_iter_free(cursorp);
        return -1;
    }

    *idxp = cursor->d++;
    *ep = cursor->i * atomic_read_32(&cursor->a->elt_align_size) +
        (unsigned char *)atomic_read_ptr((volatile void **)&cursor->a->elts);
    cursor->i++;

    return 0;
}

void
array_rope_iter_free(array_rope_cursor *cursorp)
{
    free(*cursorp);
    *cursorp = NULL;
}
