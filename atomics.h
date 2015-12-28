
#ifndef ATOMICS_H
#define ATOMICS_H

#ifdef HAVE_ILLUMOS_ATOMICS
#include <atomics.h>
#else
#include <stdint.h>
uint32_t atomic_inc_32_nv(volatile uint32_t *);
uint32_t atomic_dec_32_nv(volatile uint32_t *);

uint64_t atomic_inc_64_nv(volatile uint64_t *);
uint64_t atomic_dec_64_nv(volatile uint64_t *);

void *atomic_cas_ptr(volatile void **, void *, void *);
uint32_t atomic_cas_32(volatile uint32_t *, uint32_t, uint32_t);
uint64_t atomic_cas_64(volatile uint64_t *, uint64_t, uint64_t);

void *atomic_read_ptr(volatile void **);
uint32_t atomic_read_32(volatile uint32_t *);
uint64_t atomic_read_64(volatile uint32_t *);

void atomic_write_ptr(volatile void **, void *);
void atomic_write_32(volatile uint32_t *, uint32_t);
void atomic_write_64(volatile uint64_t *, uint64_t);
#endif

#endif /* ATOMICS_H */
