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

#ifndef THREAD_SAFE_GLOBAL_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* _np == non-portable, really, non-standard extension */


/**
 * A pthread_var_np_t is an object that holds a pointer to what would
 * typically be configuration information.  Thus a pthread_var_np_t is
 * a thread-safe "global variable".  Threads can safely read and write
 * this via pthread_var_get_np() and pthread_var_set_np(), with
 * configuration values destroyed only when the last reference is
 * released.  Applications should store mostly read-only values in a
 * pthread_var_np_t -- typically configuration information, the sort of
 * data that rarely changes.
 *
 * Writes are serialized.  Readers don't block and do not spin, and
 * mostly perform only fast atomic operations; the only blocking
 * operations done by readers are for uncontended resources.
 */
typedef struct pthread_var_np *pthread_var_np_t;

typedef void (*pthread_var_destructor_np_t)(void *);

int  pthread_var_init_np(pthread_var_np_t *, pthread_var_destructor_np_t);
void pthread_var_destroy_np(pthread_var_np_t);

int  pthread_var_get_np(pthread_var_np_t, void **, uint64_t *);
int  pthread_var_wait_np(pthread_var_np_t);
int  pthread_var_set_np(pthread_var_np_t, void *, uint64_t *);
void pthread_var_release_np(pthread_var_np_t);

#ifdef __cplusplus
}
#endif

#endif /* THREAD_SAFE_GLOBAL_H */
