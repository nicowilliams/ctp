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

#ifndef ARRAY_ROPE_H
#define ARRAY_ROPE_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A generic growable, lock-free array.
 */
typedef struct array_rope_s *array_rope;
typedef struct array_rope_cursor_s *array_rope_cursor;
typedef enum array_rope_get_options_e {
    AR_GO_IF_SET = 1,
    AR_GO_FORCE  = 2}
array_rope_get_options;

int  array_rope_init(volatile array_rope *, size_t);
void array_rope_destroy(array_rope *);

int  array_rope_add(array_rope, void **, int *);
int  array_rope_get(array_rope, array_rope_get_options, int, void **);
void *array_rope_getp(array_rope, array_rope_get_options, int);
int  array_rope_get_index(array_rope, void *);
int  array_rope_iter(array_rope, array_rope_cursor *, int *, void **);
void array_rope_iter_free(array_rope_cursor *);

#ifdef __cplusplus
}
#endif

#endif /* ARRAY_ROPE_H */
