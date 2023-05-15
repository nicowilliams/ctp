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

#ifndef ATOMICS_H
#define ATOMICS_H

#include <stdint.h>

/* Increment, decrement, and CAS with sequentally consisten ordering */
uint32_t atomic_inc_32_nv(volatile uint32_t *);
uint32_t atomic_dec_32_nv(volatile uint32_t *);

uint64_t atomic_inc_64_nv(volatile uint64_t *);
uint64_t atomic_dec_64_nv(volatile uint64_t *);

void *atomic_cas_ptr(volatile void **, const void *, const void *);
uint32_t atomic_cas_32(volatile uint32_t *, uint32_t, uint32_t);
uint64_t atomic_cas_64(volatile uint64_t *, uint64_t, uint64_t);

/* Read with at least acquire semantics */
void *atomic_read_ptr(volatile void **);
uint32_t atomic_read_32(volatile uint32_t *);
uint64_t atomic_read_64(volatile uint64_t *);

/* Write with at least release sematincs */
void atomic_write_ptr(volatile void **, const void *);
void atomic_write_32(volatile uint32_t *, uint32_t);
void atomic_write_64(volatile uint64_t *, uint64_t);

#define MAKE_ATOMICS_OF_TYPE(name, type)                            \
static inline type                                                  \
atomic_cas_ ## name(volatile type *vp, const type ov, const type nv)\
{                                                                   \
    return atomic_cas_ptr((volatile void **)vp, (void *)ov,         \
                          (void *)nv);                              \
}                                                                   \
static inline type atomic_read_ ## name(volatile type *vp)          \
{                                                                   \
    return atomic_read_ptr((volatile void **)vp);                   \
}                                                                   \
static inline void atomic_write_ ## name(volatile type *vp,         \
                                         const type nv)             \
{                                                                   \
    atomic_write_ptr((volatile void **)vp, (const void *)nv);       \
}

#define MAKE_ATOMICS_OF_TYPEDEF(typename)                                   \
    MAKE_ATOMICS_OF_TYPE(typename, typename)

#endif /* ATOMICS_H */
