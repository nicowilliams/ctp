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

#ifndef CTP_KEY_H
#define CTP_KEY_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ctp_key_s *ctp_key;
typedef void  (*ctp_key_dtor)(void *);
typedef int   (*ctp_key_create_f)(ctp_key *, ctp_key_dtor);
typedef void  (*ctp_key_delete_f)(ctp_key *);
typedef void *(*ctp_key_getspecific_f)(ctp_key);
typedef int   (*ctp_key_setspecific_f)(ctp_key, const void *);

typedef struct ctp_key_descriptor_s {
    ctp_key key;
    ctp_key_create_f mk;
    ctp_key_delete_f del;
    ctp_key_getspecific_f get;
    ctp_key_setspecific_f set;
} *ctp_key_descriptor;

int  ctp_key_create(ctp_key *, ctp_key_dtor);
void ctp_key_delete(ctp_key *);
void *ctp_key_getspecific(ctp_key);
int  ctp_key_setspecific(ctp_key, const void *);

/*
 * Wrappers around pthread keys that have the same signature as the CTP
 * pthread key multiplexer.
 */
int  ctp_key_pthread_create(ctp_key *, ctp_key_dtor);
void ctp_key_pthread_delete(ctp_key *);
void *ctp_key_pthread_getspecific(ctp_key);
int  ctp_key_pthread_setspecific(ctp_key, const void *);

#define CTP_OFFSETOF(type,memb) ((size_t)(uintptr_t)&((type*)0)->memb)
#define CTP_ALIGNOF(type) CTP_OFFSETOF(struct { char c; type member; }, member)

#define MAKE_KEY_OF_TYPE(name, type)                                \
typedef struct ctp_key_of_ ## name ## _s {                          \
    ctp_key k;                                                      \
} ctp_key_of_ ## name;                                              \
typedef void  (*ctp_key_of_ ## name ## _dtor)(void *);              \
static inline int                                                   \
ctp_key_of_ ## name ## _create(ctp_key_of_ ## name *kp,             \
                               ctp_key_of_ ## name ## _dtor dtor)   \
{                                                                   \
    return ctp_key_create(&kp->k, (ctp_key_dtor)dtor);              \
}                                                                   \
static inline void                                                  \
ctp_key_of_ ## name ## _delete(ctp_key_of_ ## name *kp)             \
{                                                                   \
    ctp_key_delete(&kp->k);                                         \
}                                                                   \
static inline type                                                  \
ctp_key_of_ ## name ## _getspecific(ctp_key_of_ ## name kp)         \
{                                                                   \
    return ctp_key_getspecific(kp->k);                              \
}                                                                   \
static inline int                                                   \
ctp_key_of_ ## name ## _setspecific(ctp_key_of_ ## name kp,         \
                                    type v)                         \
{                                                                   \
    return ctp_key_setspecific(kp->k, v);                           \
}                                                                   \

#define MAKE_KEY_OF_TYPEDEF(typename)                               \
    MAKE_KEY_OF_TYPEDEF(typename, typename)

#ifdef __cplusplus
}
#endif

#endif /* CTP_KEY_H */
