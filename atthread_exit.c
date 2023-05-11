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
#include "atthread_exit.h"
#include "atomics.h"

#define OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define ALIGNOF(type) OFFSETOF(struct { char c; type member; }, member)

static pthread_key_t key;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static array_rope handlers;

static void run_atthread_exit_handlers(void *);

struct handler_s {
    atthread_exit_handler handler;
    volatile void *data;
};

static void
once_f(void)
{
    if (pthread_key_create(&key, run_atthread_exit_handlers))
        return;
    array_rope_init(&handlers, ALIGNOF(struct handler_s));
}

static void
run_atthread_exit_handlers(void *junk)
{
    struct handler_s *e;
    array_rope_cursor c = NULL;
    void *p;
    int idx, ret;

    (void) junk;
    while ((ret = array_rope_iter(handlers, &c, &idx, &p)) == 0) {
        e = p;
        if (e->handler && (p = atomic_read_ptr(&e->data)))
            e->handler(p);
    }
    array_rope_iter_free(&c);
}

/**
 * Add a handler for thread exit.
 *
 * Both, `handler' and `data' must be non-NULL.
 */
int
atthread_exit(atthread_exit_handler handler, void *data)
{
    struct handler_s *e;
    array_rope_cursor c = NULL;
    array_rope a;
    void *p;
    int idx, ret;

    (void) pthread_once(&once, once_f);
    if ((a = atomic_read_ptr((volatile void **)&handlers)) == NULL)
        return ENOMEM; /* pthread_key_create() must have failed */

    while ((ret = array_rope_iter(handlers, &c, &idx, &p)) == 0) {
        e = p;
        if (e->handler == handler && e->data)
            return 0;
    }
    array_rope_iter_free(&c);
    if (ret > 0)
        return ret;
    if ((ret = array_rope_add(handlers, &p, &idx)))
        return ret;
    e = p;
    e->handler = handler;
    atomic_write_ptr(&e->data, data);
    return 0;
}

/**
 * Remove a handler for thread exit (e.g., because of shared object
 * unloading).
 *
 * Both, `handler' and `data' must be non-NULL.
 */
int
atthread_exit_remove(atthread_exit_handler handler, void *data)
{
    struct handler_s *e;
    array_rope_cursor c = NULL;
    array_rope a;
    void *p;
    int idx, ret;

    if ((a = atomic_read_ptr((volatile void **)&handlers)) == NULL)
        return 0;

    while ((ret = array_rope_iter(handlers, &c, &idx, &p)) == 0) {
        e = p;
        if (e->handler == handler && atomic_read_ptr(&e->data) == data) {
            e->handler = NULL;
            atomic_write_ptr(&e->data, NULL);
            return 0;
        }
    }
    return ret;
}
