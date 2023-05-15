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

#ifndef DESC_TBL_H
#define DESC_TBL_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A descriptor table data structure, much like a file descriptor table,
 * but generic.
 */
typedef struct desc_tbl_s *desc_tbl;
typedef struct desc_tbl_elt_s *desc_tbl_elt;
typedef struct desc_tbl_cursor_s *desc_tbl_cursor;

typedef int (*desc_tbl_close_f)(volatile void *);

int  desc_tbl_init(volatile desc_tbl *, desc_tbl_close_f);
void desc_tbl_destroy(desc_tbl *);

int  desc_tbl_open(desc_tbl, void *, desc_tbl_elt *, int *, uint64_t *);
int  desc_tbl_get_p(desc_tbl, desc_tbl_elt, uint64_t, void **);
int  desc_tbl_get_n(desc_tbl, int, uint64_t, desc_tbl_elt *, void **);
void desc_tbl_put(desc_tbl);
int  desc_tbl_iter(desc_tbl, desc_tbl_cursor *, int *, uint64_t *, desc_tbl_elt *, void **);
void desc_tbl_iter_free(desc_tbl_cursor *);
int  desc_tbl_close_p(desc_tbl, desc_tbl_elt, uint64_t);
int  desc_tbl_close_n(desc_tbl, int, uint64_t);

#define CTP_OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define CTP_ALIGNOF(type) CTP_OFFSETOF(struct { char c; type member; }, member)

#define MAKE_DESC_TBL_OF_TYPE(name, type)                               \
typedef struct desc_tbl_of_ ## name ## _s {                             \
    desc_tbl t;                                                         \
} desc_tbl_of_ ## name;                                                 \
typedef struct desc_tbl_of_ ## name ## _elt_s {                         \
    desc_tbl_elt e;                                                     \
} desc_tbl_of_ ## name ## _elt;                                         \
typedef struct desc_tbl_of_ ## name ## _cursor_s {                      \
    desc_tbl_cursor c;                                                  \
} desc_tbl_of_ ## name ## _cursor;                                      \
typedef int (*desc_tbl_of_ ## name ## _close_f)(volatile void *);       \
static inline int                                                       \
desc_tbl_of_ ## name ## _init(volatile desc_tbl_of_ ## name *tbl,       \
                             desc_tbl_of_ ## name ## _close_f closef)   \
{                                                                       \
    return desc_tbl_init(&tbl->a, (desc_tbl_close_f)closef);            \
}                                                                       \
static inline void                                                      \
desc_tbl_of_ ## name ## _destroy(desc_tbl_of_ ## name *tbl)             \
{                                                                       \
    desc_tbl_destroy(tbl.t);                                            \
}                                                                       \
static inline int                                                       \
desc_tbl_of_ ## name ## _open(desc_tbl_of_ ## name tbl,                 \
                             type v,                                    \
                             desc_tbl_of_ ## name ## _elt *ep,          \
                             int *idxp,                                 \
                             uint64_t *verifierp)                       \
{                                                                       \
    return desc_tbl_open(tbl.t, v, &ep->e, idxp, verifierp);            \
}                                                                       \
static inline int                                                       \
desc_tbl_of_ ## name ## _get_p(desc_tbl_of_ ## name tbl,                \
                              desc_tbl_of_ ## name ## _elt e,           \
                              uint64_t verifier,                        \
                              type *vp)                                 \
{                                                                       \
    desc_tbl_get_p(tbl.t, e.e, verifier, (void **)vp);                  \
}                                                                       \
static inline int                                                       \
desc_tbl_of_ ## name ## _get_n(desc_tbl_of_ ## name tbl,                \
                              int idx,                                  \
                              uint64_t verifier,                        \
                              desc_tbl_of_ ## name ## _elt *ep,         \
                              type *vp)                                 \
{                                                                       \
    return desc_tbl_get_n(tbl.t, idx, verifier, &ep->e, (void **)vp);   \
}                                                                       \
static inline void                                                      \
desc_tbl_of_ ## name ## _put(desc_tbl_of_ ## name);                     \
{                                                                       \
    desc_tbl_put(tbl.t);                                                \
}                                                                       \
static inline int                                                       \
desc_tbl_of_ ## name ## _iter(desc_tbl_of_ ## name tbl,                 \
                             desc_tbl_of_ ## name ## _cursor *cp,       \
                             int *idxp,                                 \
                             uint64_t *verifierp,                       \
                             desc_tbl_of_ ## name ## _elt *ep,          \
                             type *vp)                                  \
{                                                                       \
    return desc_tbl_iter(tbl.t, &cp.c, idxp, verifierp, &ep->e,         \
                         (void **)vp);                                  \
}                                                                       \
static inline void                                                      \
desc_tbl_of_ ## name ## _iter_free(desc_tbl_of_ ## name ## _cursor *cp) \
{                                                                       \
    desc_tbl_iter_free(&cp.c);                                          \
}                                                                       \
static inline int                                                       \
desc_tbl_of_ ## name ## _close_p(desc_tbl_of_ ## name tbl,              \
                                 desc_tbl_of_ ## name ## _elt e,        \
                                 uint64_t verifier)                     \
{                                                                       \
    return desc_tbl_close_p(tbl.t, e.e, verifier);                      \
}                                                                       \
static inline int                                                       \
desc_tbl_of_ ## name ## _close_n(desc_tbl_of_ ## name tbl,              \
                                 int idx,                               \
                                 uint64_t verifier)                     \
{                                                                       \
    return desc_tbl_close_n(tbl.a, idx, verifier);                      \
}                                                                       \

#define MAKE_DESC_TBL_OF_TYPEDEF(typename)                          \
    MAKE_DESC_TBL_OF_TYPEDEF(typename, typename)


#ifdef __cplusplus
}
#endif

#endif /* DESC_TBL_H */
