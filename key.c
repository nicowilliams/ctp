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

#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "array_rope.h"
#include "desc_tbl.h"
#include "atthread_exit.h"
#include "key.h"
#include "atomics.h"

#define OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define ALIGNOF(type) OFFSETOF(struct { char c; type member; }, member)

static pthread_key_t pkey;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static desc_tbl keys;
static int desc_tbl_init_ret;

static void cleanup(void *);

static void
once_f(void)
{
    if ((desc_tbl_init_ret = pthread_key_create(&pkey, cleanup)) == 0)
        desc_tbl_init_ret = desc_tbl_init(&keys, NULL);
}

typedef struct ctp_pkey *ctp_pkey;

struct ctp_key_s {
    ctp_key_dtor dtor;
    int idx;            /* Index into the pthread_key's value that is an
                         * array of thread-local values for CTP keys */
    desc_tbl_elt e;     /* The element of the descriptor table */
    uint64_t v;         /* Descriptor verifier */
    ctp_pkey pk;        /* All the threads that have used this key */
};

typedef struct ctp_pkey_value_s {
    ctp_key key;
    void *value;
} *ctp_pkey_value;

/* The value type of the one pthread_key we use */
struct ctp_pkey {
    ctp_key key;
    array_rope a;
    ctp_pkey next;  /* Next thread's values (needed for destructing) */
};

int
ctp_key_create(ctp_key *keyp, ctp_key_dtor dtor)
{
    ctp_key k;
    int ret;

    (void) pthread_once(&once, once_f);
    if (keys == NULL)
        return desc_tbl_init_ret;
    if ((k = calloc(1, sizeof(*k))) == NULL)
        return ENOMEM;

    k->dtor = dtor;
    if ((ret = desc_tbl_open(keys, k, &k->e, &k->idx, &k->v)))
        return ret;
    atomic_write_ptr((volatile void **)keyp, k);
    return 0;
}

void
ctp_key_delete(ctp_key *kp)
{
    ctp_pkey_value v;
    ctp_pkey pk;
    ctp_key k = *kp;

    *kp = NULL;
    if (k->idx) {
        for (pk = atomic_read_ptr((volatile void **)&k->pk);
             pk;
             pk = atomic_read_ptr((volatile void **)&pk->next)) {
            if (k->dtor) {
                ctp_pkey_value v = array_rope_getp(pk->a, AR_GO_IF_SET, k->idx);
                if (v)
                    k->dtor(v->value);
            }
            if ((v = array_rope_getp(pk->a, AR_GO_IF_SET, k->idx))) {
                atomic_write_ptr((volatile void **)&v->key, NULL);
                atomic_write_ptr((volatile void **)&v->value, NULL);
            }
        }
    }
    free(*kp);
}

void *
ctp_key_getspecific(ctp_key k)
{
    ctp_pkey_value v;
    ctp_pkey pk;

    if ((pk = pthread_getspecific(pkey)) == NULL ||
        (v = array_rope_getp(pk->a, AR_GO_IF_SET, k->idx)) == NULL ||
        v->key != k)
        return NULL;
    return v->value;
}

int
ctp_key_setspecific(ctp_key k, const void *value)
{
    ctp_pkey_value v;
    ctp_pkey pk;
    int ret;

    if (k->idx < 0)
        return EINVAL;

    if ((pk = pthread_getspecific(pkey)) == NULL) {
        if ((pk = calloc(1, sizeof(*pk))) == NULL)
            return ENOMEM;

        pk->key = k;

        /*
         * This is the array of ctp_key values we'll be indexing with
         * `k->idx'.
         */
        if ((ret = array_rope_init(&pk->a, ALIGNOF(struct ctp_pkey_value_s))))
            return ret;

        /* Save it */
        if ((ret = pthread_setspecific(pkey, pk)))
            return ret;

        /* Link it in */
        for (pk->next = atomic_read_ptr((volatile void **)&k->pk);
             atomic_cas_ptr((volatile void **)&k->pk, pk->next, pk);
             pk->next = atomic_read_ptr((volatile void **)&k->pk))
            ;
    }

    if ((ret = array_rope_get(pk->a, AR_GO_FORCE, k->idx, (void **)&v)))
        return ret;

    atomic_write_ptr((volatile void **)&v->key, k);
    atomic_write_ptr((volatile void **)&v->value, value);
    return 0;
}

static void
cleanup(void *data)
{
    array_rope_cursor c = NULL;
    ctp_pkey pk = data;
    void *v;
    int idx;

    while (array_rope_iter(pk->a, &c, &idx, &v) == 0)
        pk->key->dtor(v);
    array_rope_iter_free(&c);
}
