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

#ifndef CTP_HAZARDS_H
#define CTP_HAZARDS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "key.h"

typedef void (*ctp_hazards_dtor_f)(void *);
typedef struct ctp_hazards_s *ctp_hazards;

/* Hazard pointers  */
struct ctp_hazards_s {
    volatile void *value;
    volatile ctp_hazards next;
    volatile uint32_t inuse;
};

void ctp_hazards_gc(ctp_hazards *, void *, ctp_hazards_dtor_f);
void ctp_hazards_thread_exit(ctp_hazards);
void ctp_hazards_destroy(ctp_hazards *, ctp_hazards_dtor_f);
ctp_hazards ctp_hazards_get(volatile ctp_hazards *, void *,
                            ctp_key_getspecific_f,
                            ctp_key_setspecific_f);
void *ctp_hazards_take(ctp_hazards, volatile void **);
void ctp_hazards_put(ctp_hazards);

#define CTP_OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define CTP_ALIGNOF(type) CTP_OFFSETOF(struct { char c; type member; }, member)

#define MAKE_HAZARDS_OF_TYPE(name, type)                                \
typedef struct ctp_hazards_of_ ## name ## _s {                          \
    ctp_hazards h;                                                      \
} ctp_hazards_of_ ## name;                                              \
typedef void  (*ctp_hazards_of_ ## name ## _dtor_f)(void *);            \
static inline int                                                       \
ctp_hazards_of_ ## name ## _gc (ctp_hazards_of_ ## name *hp,            \
                               void *v,                                 \
                               ctp_hazards_of_ ## name ## _dtor_f dtor) \
{                                                                       \
    return ctp_hazards_gc((ctp_hazards*)hp, v,                          \
                          (ctp_hazards_dtor_f)dtor);                    \
}                                                                       \
static inline void                                                      \
ctp_hazards_of_ ## name ## _thread_exit(ctp_hazards_of_ ## name h)      \
{                                                                       \
    ctp_hazards_thread_exit(h.h);                                       \
}                                                                       \
static inline void                                                      \
ctp_hazards_of_ ## name ## _destroy(ctp_hazards_of_ ## name *hp,        \
                                   ctp_hazards_of_ ## name ## _dtor_f dtor) \
{                                                                       \
    ctp_hazards_destroy(&h.h, (ctp_hazards_dtor_f)dtor);                \
}                                                                       \
static inline int                                                       \
ctp_hazards_of_ ## name                                                 \
ctp_hazards_of_ ## name _get(volatile ctp_hazards_of_ ## name *hp,      \
                            type v,                                     \
                            ctp_key_getspecific_f get,                  \
                            ctp_key_setspecific_f set)                  \
{                                                                       \
    return ctp_hazards_get((volatile ctp_hazards **)hp,                 \
                           vp, get, set);                               \
}                                                                       \
static inline void *                                                    \
ctp_hazards_of_ ##take(ctp_hazards_of_ ## name h, volatile type *vp)    \
{                                                                       \
    return ctp_hazards_take(h.h, (volatile void **)vp);                 \
}                                                                       \
static inline void                                                      \
ctp_hazards_put(ctp_hazards)                                            \
{                                                                       \
    ctp_hazards_put(h.h);                                               \
}                                                                       \

#define MAKE_HAZARDS_OF_TYPEDEF(typename)                           \
    MAKE_HAZARDS_OF_TYPEDEF(typename, typename)

#ifdef __cplusplus
}
#endif

#endif /* CTP_HAZARDS_H */
