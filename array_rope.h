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

#define CTP_OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define CTP_ALIGNOF(type) CTP_OFFSETOF(struct { char c; type member; }, member)

#define MAKE_ARRAY_OF_TYPE(name, type)                              \
static inline int                                                   \
array_rope_of_ ## name ## _init(volatile array_rope *ap)            \
{                                                                   \
    return array_rope_init(ap, CTP_ALIGNOF(type));                  \
}                                                                   \
static inline int                                                   \
array_rope_of_ ## name ## _add(array_rope a, type *v, int *idx)     \
{                                                                   \
    return array_rope_add(a, (void *v), idx);                       \
}                                                                   \
static inline int                                                   \
array_rope_of_ ## name ## _get(array_rope a,                        \
                               array_rope_get_options opt,          \
                               int idx, type *vp)                   \
{                                                                   \
    return array_rope_get(a, opt, idx, (void **)vp);                \
}                                                                   \
static inline type                                                  \
array_rope_of_ ## name ## _getp(array_rope a,                       \
                                array_rope_get_options opt,         \
                                int idx)                            \
{                                                                   \
    return array_rope_getp(a, opt, idx);                            \
}                                                                   \
static inline int                                                   \
array_rope_of_ ## name ## _get_index(array_rope a, type v)          \
{                                                                   \
    return array_rope_get_index(a, v);                              \
}                                                                   \
static inline int                                                   \
array_rope_of_ ## name ## _iter(array_rope a,                       \
                                array_rope_cursor *c,               \
                                int *idxp,                          \
                                type *vp)                           \
{                                                                   \
    return array_rope_iter(a, c, idxp, (void **)vp);                \
}                                                                   \
static inline void                                                  \
array_rope_of_ ## name ## _iter_free(array_rope_cursor *c)          \
{                                                                   \
    array_rope_iter_free(c);                                        \
}                                                                   \

#define MAKE_ARRAY_OF_TYPEDEF(typename)                             \
    MAKE_ARRAY_OF_TYPEDEF(typename, typename)


#ifdef __cplusplus
}
#endif

#endif /* ARRAY_ROPE_H */
