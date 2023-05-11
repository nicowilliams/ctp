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

/*
 * This implements a thread-safe variable.  A thread can read it, and the
 * value it reads will be safe to continue using until it reads it again.
 *
 * Properties:
 *
 *  - writers are serialized
 *  - readers are fast, rarely doing blocking operations, and when they
 *    do, not blocking on contended resources (in one of two
 *    implementations below readers never block, not even on uncontended
 *    resources)
 *  - readers do not starve writers; writers do not block readers
 */

#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "thread_safe_global.h"
#include "atomics.h"

#if defined(USE_TSV_SLOT_PAIR_DESIGN) && defined(USE_TSV_SUBSCRIPTION_SLOTS_DESIGN)
#error "Must define only one of USE_TSV_SLOT_PAIR_DESIGN or USE_TSV_SUBSCRIPTION_SLOTS_DESIGN"
#endif

#if !defined(USE_TSV_SLOT_PAIR_DESIGN) && !defined(USE_TSV_SUBSCRIPTION_SLOTS_DESIGN)
#define USE_TSV_SLOT_PAIR_DESIGN
#endif

typedef thread_safe_var_dtor_f var_dtor_t;

#if 0
static thread_safe_var *get_var(uint32_t);
static thread_safe_var *new_var(uint32_t *);
#endif

/*
 * There are two designs, but one of them is ommited here.
 *
 * See https://github.com/nicowilliams/ctp
 */

#ifdef USE_TSV_SLOT_PAIR_DESIGN
/*
 * This implements a thread-safe variable.  A thread can read it, and the
 * value it reads will be safe to continue using until it reads it again.
 *
 * Properties:
 *
 *  - writers are serialized
 *  - readers are fast, rarely doing blocking operations, and when they
 *    do, not blocking on contended resources (in one of two
 *    implementations below readers never block, not even on uncontended
 *    resources)
 *  - readers do not starve writers; writers do not block readers
 *
 */

/*
 * The design for this implementation uses a pair of slots such that one
 * has a current value for the variable, and the other holds the
 * previous value and will hold the next value.
 *
 * There are several atomic compositions needed to make this work.
 *
 *  - writers have to write two things (a pointer to a struct wrapping
 *    the intended value, and a version number)
 *
 *  - readers have to atomically read a version number, a pointer, and
 *    increment a ref count.
 *
 * These compositions are the challenging part of this.
 *
 * In a way this structure is a lot like a read-write lock that doesn't
 * starve writers.  But since the only thing readers here do with a
 * would-be read-write lock held is grab a reference to a "current"
 * value, this construction can be faster than read-write locks without
 * writer starvation: readers (almost) *never* block on contended
 * resources.  We achieve this by having two value slots: one for the
 * current value, and one for the previous/next value.  Readers can
 * always lock-less-ly find one of the two values.
 *
 * Whereas in the case of a read-write lock without writer starvation,
 * readers arriving after a writer must get held up for the writer who,
 * in turn, is held up by readers.  Therefore, for the typical case
 * where one uses read-write locks (to mediate access to rarely-changing
 * mostly-read-only data, typically configuration data), the API
 * implemented here is superior to read-write locks.
 *
 * We often use atomic CAS with equal new and old values as an atomic
 * read.  We could do better though.  We could use a LoadStore fence
 * around reads instead.
 *
 * NOTE WELL: We assume that atomic operations imply memory barriers.
 *
 *            The general rule is that all things which are to be
 *            atomically modified in some cases are always modified
 *            atomically, except at initialization time, and even then,
 *            in some cases the initialized value is immediately
 *            modified with an atomic operation.  This is to ensure
 *            memory visibility rules (see above), though we may be
 *            trying much too hard in some cases.
 *
 *            The atomic operations from atomics.[ch] provide the necessary
 *            barriers.
 */

/*
 * This design uses a pair of "slots", such that one holds the current value of
 * the thread-safe global variable, while the other holds the previous/next
 * value.
 *
 * Writers make the previous slot into the new current slot, being careful not
 * to step on the toes of a reader that was reading from that slot thinking it
 * was the current slot.
 *
 * Readers are lock-less, except that when a reader is the last reader of a
 * slot it has to signal a writer that might be waiting for that reader to be
 * done with the slot.  Also, readers do call free(), which may acquire locks.
 * Sending that signal requires taking a lock that the writer will have dropped
 * in order to wait, thus it should be an uncontended lock, and if the reader
 * blocks racing with a writer, it should unblock very soon after.  This is
 * never needed when the value has not changed since the previous read.
 *
 * Both, reading, and writing are O(1).
 */

/*
 * Values set on a thread-global variable are wrapped with a struct that
 * holds a reference count.
 */
struct vwrapper {
    var_dtor_t          dtor;       /* value destructor */
    void                *ptr;       /* the actual value */
    uint64_t            version;    /* version of this data */
    volatile uint32_t   nref;       /* release when drops to 0 */
};

/* This is a slot.  There are two of these. */
struct var {
    struct vwrapper     *wrapper;   /* wraps real ptr, has nref */
    struct var          *other;     /* always points to the other slot */
    uint64_t            version;    /* version of this slot's data */
    volatile uint32_t   nreaders;   /* no. of readers active in this slot */
};

struct thread_safe_var_s {
    pthread_key_t       tkey;           /* to detect thread exits */
    pthread_mutex_t     write_lock;     /* one writer at a time */
    pthread_mutex_t     waiter_lock;    /* to signal waiters */
    pthread_cond_t      waiter_cv;      /* to signal waiters */
    pthread_mutex_t     cv_lock;        /* to signal waiting writer */
    pthread_cond_t      cv;             /* to signal waiting writer */
    struct var          vars[2];        /* the two slots */
    var_dtor_t          dtor;           /* both read this */
    uint64_t            next_version;   /* both read; writer writes */
};


static void
wrapper_free(struct vwrapper *wrapper)
{
    if (wrapper == NULL)
        return;
    if (atomic_dec_32_nv(&wrapper->nref) > 0)
        return;
    if (wrapper->dtor != NULL)
        wrapper->dtor(wrapper->ptr);
    free(wrapper);
}

/* For the thread-specific key */
static void
var_dtor_wrapper(void *wrapper)
{
    wrapper_free(wrapper);
}

/**
 * Initialize a thread-safe global variable
 *
 * A thread-safe global variable stores a current value, a pointer to
 * void, which may be set and read.  A value read from a thread-safe
 * global variable will be valid in the thread that read it, and will
 * remain valid until released or until the thread-safe global variable
 * is read again in the same thread.  New values may be set.  Values
 * will be destroyed with the destructor provided when no references
 * remain.
 *
 * @param var Pointer to thread-safe global variable
 * @param dtor Pointer to thread-safe global value destructor function
 *
 * @return Returns zero on success, else a system error number
 */
int
thread_safe_var_init(thread_safe_var *vpp,
                     thread_safe_var_dtor_f dtor)
{
    thread_safe_var vp;
    int err;

    *vpp = NULL;
    if ((vp = calloc(1, sizeof(*vp))) == NULL)
        return errno;

    /*
     * The thread-local key is used to hold a reference for destruction
     * at thread-exit time, if the thread does not explicitly drop the
     * reference before then.
     *
     * There's no pthread_key_destroy(), so we leak these.  We ought to
     * have a single global thread key whose values point to a
     * counted-length array of keys, with a global counter that we
     * snapshot here and use as an index into that array (and which is
     * realloc()'ed as needed).
     */
    if ((err = pthread_key_create(&vp->tkey, var_dtor_wrapper)) != 0) {
        free(vp);
        return err;
    }
    if ((err = pthread_mutex_init(&vp->write_lock, NULL)) != 0) {
        free(vp);
        return err;
    }
    if ((err = pthread_mutex_init(&vp->waiter_lock, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        free(vp);
        return err;
    }
    if ((err = pthread_mutex_init(&vp->cv_lock, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        pthread_mutex_destroy(&vp->cv_lock);
        free(vp);
        return err;
    }
    if ((err = pthread_cond_init(&vp->cv, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        pthread_mutex_destroy(&vp->waiter_lock);
        pthread_mutex_destroy(&vp->cv_lock);
        free(vp);
        return err;
    }
    if ((err = pthread_cond_init(&vp->waiter_cv, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        pthread_mutex_destroy(&vp->waiter_lock);
        pthread_mutex_destroy(&vp->cv_lock);
        pthread_cond_destroy(&vp->cv);
        free(vp);
        return err;
    }

    /*
     * vp->next_version is a 64-bit unsigned int.  If ever we can't get
     * atomics to deal with it on 32-bit platforms we could have a
     * pointer to one of two version numbers which are not atomically
     * updated, and instead atomically update the pointer.
     */
    vp->next_version = 0;
    vp->vars[0].nreaders = 0;
    vp->vars[0].wrapper = NULL;
    vp->vars[0].other = &vp->vars[1]; /* other pointer never changes */
    vp->vars[1].nreaders = 0;
    vp->vars[1].wrapper = NULL;
    vp->vars[1].other = &vp->vars[0]; /* other pointer never changes */
    vp->dtor = dtor;

    /*
     * Acquiring and dropping the lock functions as a trivial memory
     * barrier.
     */
    pthread_mutex_lock(&vp->write_lock);
    *vpp = vp;
    pthread_mutex_unlock(&vp->write_lock);
    return 0;
}

/**
 * Destroy a thread-safe global variable
 *
 * It is the caller's responsibility to ensure that no thread is using
 * this var and that none will use it again.
 *
 * @param [in] var The thread-safe global variable to destroy
 */
void
thread_safe_var_destroy(thread_safe_var vp)
{
    if (vp == 0)
        return;

    thread_safe_var_release(vp);
    pthread_mutex_lock(&vp->write_lock); /* There'd better not be readers */
    pthread_cond_destroy(&vp->cv);
    pthread_mutex_destroy(&vp->cv_lock);
    wrapper_free(vp->vars[0].wrapper);
    wrapper_free(vp->vars[1].wrapper);
    vp->vars[0].other = &vp->vars[1];
    vp->vars[1].other = &vp->vars[0];
    vp->vars[0].wrapper = NULL;
    vp->vars[1].wrapper = NULL;
    vp->dtor = NULL;
    pthread_mutex_unlock(&vp->write_lock);
    pthread_mutex_destroy(&vp->write_lock);
    free(vp);
    /* Remaining references will be released by the thread key destructor */
    /* XXX We leak var->tkey!  See note in initiator above. */
}

static int
signal_writer(thread_safe_var vp)
{
    int err;

    if ((err = pthread_mutex_lock(&vp->cv_lock)) != 0)
        return err;
    if ((err = pthread_cond_signal(&vp->cv)) != 0)
        abort();
    return pthread_mutex_unlock(&vp->cv_lock);
}

/**
 * Get the most up to date value of the given cf var.
 *
 * @param [in] var Pointer to a cf var
 * @param [out] res Pointer to location where the variable's value will be output
 * @param [out] version Pointer (may be NULL) to 64-bit integer where the current version will be output
 *
 * @return Zero on success, a system error code otherwise
 */
int
thread_safe_var_get(thread_safe_var vp, void **res, uint64_t *version)
{
    int err = 0;
    int err2 = 0;
    uint32_t nref;
    struct var *v;
    uint64_t vers;
    struct vwrapper *wrapper;

    if (version == NULL)
        version = &vers;
    *version = 0;

    *res = NULL;

    if ((wrapper = pthread_getspecific(vp->tkey)) != NULL &&
        wrapper->version == atomic_read_64(&vp->next_version) - 1) {

        /* Fast path */
        *version = wrapper->version;
        *res = wrapper->ptr;
        return 0;
    }

    /* Busy loop to get current slot.  Races with writers. */
    for (;;) {
        /* Get the current next_version */
        *version = atomic_read_64(&vp->next_version);
        if (*version == 0)
            return 0;
        (*version)--; /* The current version is one less than next_version */

        /* Get what we hope is still the current slot */
        v = &vp->vars[(*version) & 0x1];

        /*
         * We picked a slot, but we could just have lost against one or more
         * writers.  So far nothing we've done would block any number of
         * them.
         *
         * We increment nreaders for the slot we picked to keep out
         * subsequent writers; we can then lose one more race at most.
         */
        (void) atomic_inc_32_nv(&v->nreaders);
        /* Repeat until we're done losing any races */
        if (atomic_read_64(&vp->next_version) == (*version + 1))
            break;
        if (atomic_dec_32_nv(&v->nreaders) == 0)
            (void) signal_writer(vp);
    }

    assert(v->wrapper != NULL);
    assert(*version + 1 == atomic_read_64(&vp->next_version) ||
           *version + 2 == atomic_read_64(&vp->next_version));

    /* Take the wrapped value for the slot we chose */
    nref = atomic_inc_32_nv(&v->wrapper->nref);
    assert(nref > 1);
    *version = v->wrapper->version;
    *res = v->wrapper->ptr;


    /*
     * Release the slot and signal any possible waiting writer if the slot's
     * nreaders drops to zero (that's what the writer will be waiting for).
     *
     * The one blocking operation done by readers happens in
     * signal_writer(), but that one blocking operation is for a lock
     * that the writer will have or will soon have released, so it's
     * a practically uncontended blocking operation.
     */
    wrapper = v->wrapper;
    if (atomic_dec_32_nv(&v->nreaders) == 0 &&
        atomic_read_64(&vp->next_version) != (*version + 1))
        err2 = signal_writer(vp);

    /*
     * Release the value previously read in this thread, if any.
     *
     * Note that we call free() here, which means that we might take a
     * lock in free().  The application's value destructor also can do
     * the same.
     *
     * TODO We could use a lock-less queue/stack to queue up wrappers
     *      for destruction by writers, then readers could be even more
     *      light-weight.  But then while synchronous value destruction could
     *      be valuable.
     */
    if (wrapper != pthread_getspecific(vp->tkey))
        thread_safe_var_release(vp);

    /* Recall this value we just read */
    err = pthread_setspecific(vp->tkey, wrapper);
    return (err2 == 0) ? err : err2;
}

/**
 * Release this thread's reference (if it holds one) to the current
 * value of the given thread-safe global variable.
 *
 * @param vp [in] A thread-safe global variable
 */
void
thread_safe_var_release(thread_safe_var vp)
{
    struct vwrapper *wrapper = pthread_getspecific(vp->tkey);

    if (wrapper == NULL)
        return;
    if (pthread_setspecific(vp->tkey, NULL) != 0)
        abort();
    assert(pthread_getspecific(vp->tkey) == NULL);
    wrapper_free(wrapper);
}

/**
 * Set new data on a thread-safe global variable
 *
 * @param [in] var Pointer to thread-safe global variable
 * @param [in] cfdata New value for the thread-safe global variable
 * @param [out] new_version New version number
 *
 * @return 0 on success, or a system error such as ENOMEM.
 */
int
thread_safe_var_set(thread_safe_var vp, void *cfdata,
                    uint64_t *new_version)
{
    int err;
    size_t i;
    struct var *v;
    struct vwrapper *old_wrapper = NULL;
    struct vwrapper *wrapper;
    struct vwrapper *tmp;
    uint64_t vers;
    uint64_t tmp_version;
    uint64_t nref;

    if (cfdata == NULL)
        return EINVAL;

    if (new_version == NULL)
        new_version = &vers;

    *new_version = 0;

    /* Build a wrapper for the new value */
    if ((wrapper = calloc(1, sizeof(*wrapper))) == NULL)
        return errno;

    /*
     * The var itself holds a reference to the current value, thus its
     * nref starts at 1, but that is made so further below.
     */
    wrapper->dtor = vp->dtor;
    wrapper->nref = 0;
    wrapper->ptr = cfdata;

    /* This functions as a memory barrier for the above writes */
    if ((err = pthread_mutex_lock(&vp->write_lock)) != 0) {
        free(wrapper);
        return err;
    }

    /* vp->next_version is stable because we hold the write_lock */
    *new_version = wrapper->version = atomic_read_64(&vp->next_version);

    /* Grab the next slot */
    v = vp->vars[(*new_version + 1) & 0x1].other;
    old_wrapper = atomic_read_ptr((volatile void **)&v->wrapper);

    if (*new_version == 0) {
        /* This is the first write; set wrapper on both slots */

        for (i = 0; i < sizeof(vp->vars)/sizeof(vp->vars[0]); i++) {
            v = &vp->vars[i];
            nref = atomic_inc_32_nv(&wrapper->nref);
            v->version = 0;
            /* This functions as a memory barrier for the above writes */
            tmp = atomic_cas_ptr((volatile void **)&v->wrapper,
                                 old_wrapper, wrapper);
            assert(tmp == old_wrapper && tmp == NULL);
        }

        assert(nref > 1);

        tmp_version = atomic_inc_64_nv(&vp->next_version);
        assert(tmp_version == 1);

        /* Signal waiters */
        (void) pthread_mutex_lock(&vp->waiter_lock);
        (void) pthread_cond_signal(&vp->waiter_cv); /* no thundering herd */
        (void) pthread_mutex_unlock(&vp->waiter_lock);
        return pthread_mutex_unlock(&vp->write_lock);
    }

    nref = atomic_inc_32_nv(&wrapper->nref);
    assert(nref == 1);

    assert(old_wrapper != NULL && atomic_read_32(&old_wrapper->nref) > 0);

    /* Wait until that slot is quiescent before mutating it */
    if ((err = pthread_mutex_lock(&vp->cv_lock)) != 0) {
        (void) pthread_mutex_unlock(&vp->write_lock);
        free(wrapper);
        return err;
    }
    while (atomic_read_32(&v->nreaders) > 0) {
        /*
         * We have a separate lock for writing vs. waiting so that no
         * other writer can steal our march.  All writers will enter,
         * all writers will finish.  We got here by winning the race for
         * the writer lock, so we'll hold onto it, and thus avoid having
         * to restart here.
         */
        if ((err = pthread_cond_wait(&vp->cv, &vp->cv_lock)) != 0) {
            (void) pthread_mutex_unlock(&vp->cv_lock);
            (void) pthread_mutex_unlock(&vp->write_lock);
            free(wrapper);
            return err;
        }
    }
    if ((err = pthread_mutex_unlock(&vp->cv_lock)) != 0) {
        (void) pthread_mutex_unlock(&vp->write_lock);
        free(wrapper);
        return err;
    }

    /* Update that now quiescent slot; these are the release operations */
    tmp = atomic_cas_ptr((volatile void **)&v->wrapper, old_wrapper, wrapper);
    assert(tmp == old_wrapper);
    v->version = *new_version;
    tmp_version = atomic_inc_64_nv(&vp->next_version); /* Memory barrier */
    assert(tmp_version == *new_version + 1);
    assert(v->version > v->other->version);

    /* Release the old cf */
    assert(old_wrapper != NULL && atomic_read_32(&old_wrapper->nref) > 0);
    wrapper_free(old_wrapper);

    /* Done */
    return pthread_mutex_unlock(&vp->write_lock);
}

#else /* USE_TSV_SLOT_PAIR_DESIGN */

#include <sched.h>

/*
 * Subscription Slot Design
 *
 * Here we have a linked list of extant values where the head has the
 * current value and where the head and the next pointers are the only
 * things written to by the writer; all readers "subscribe" and thence
 * their state consists of a single pointer to what was at read-time the
 * head of that linked list, with that pointer held where the writers
 * can find it (in "subscription" slots) so they can garbage collect in
 * order to release no-longer referenced values.
 *
 * Subscription is lock-less.  There's an index into a logical array of
 * subscription slots.  New reader threads increment a counter to
 * determine their index into this array.  If the array index goes past
 * the allocated array size, then the array is grown lock-less-ly.  The
 * array is maintained as a linked list of array chunks; when a reader
 * goes to grow it, it will either win a race to grow it or lose it,
 * using an atomic CAS operation to perform the growth; losers free
 * their chunk and then look for their slot in the winner's chunk and
 * possibly retry the array growth operation.
 *
 * Once subcribed, readers only ever do an acquire-fenced read on the
 * head of the linked list of values, and write that to their slot with
 * a release-fenced write.
 *
 * Writers only add new values at the head of the list, with the
 * previous head as the next pointer of the new element.
 *
 * Writers also mark-and-sweep garbage collect the extant value list by
 * reading every subscribed thread's pointer with an acquire-fenced
 * read, marking all in-use values as such, then the writer releases and
 * removes from the list those elements not marked as in-used.  Readers
 * never read the next pointers of the list's elements.
 *
 * Readers do two fenced memory operations.  Writers do N fenced memory
 * operations plus the writer lock acquire/release and any locks
 * required to allocate and free list elements.  Readers may have to
 * allocate the first time they read, but not thereafter.
 */

/*
 * Design #2: Value list + per-reader thread slots.
 *
 * This design uses a list of referenced values and a set of slots, one per
 * thread that has read this thread-safe global variable.
 *
 * Readers "subscribe" the first time they read a thread-safe global variable,
 * allocating a slot.  Thereafter readers are very fast, using two fenced
 * memory operations to get the newest value of the thread-safe global
 * variable.
 *
 * Writers add new values to the head of a linked list, then garbage collect
 * the list by visiting all the reader subscription slots to mark the list then
 * sweep it.
 *
 * Readers never ever block and never call into the allocator.  First time
 * readers are O(N), else they are O(1).  Compare to the two-slot design where
 * readers may block briefly but are always O(1).
 *
 * Writers are serialized but do not block while holding the lock.  Writers do
 * not call the allocator while holding the lock.  Writers are O(N).  Compare to
 * the two-slot design, where writers are O(1).
 */

/* This is an element on the list of referenced values */
struct value {
    volatile struct value   *next;      /* previous (still ref'd) value */
    void                    *value;     /* actual value */
    volatile uint64_t       version;    /* version number */
    volatile uint32_t       referenced; /* for mark and sweep */
};

/*
 * Each thread that has read this thread-safe global variable gets one
 * of these.
 */
struct slot {
    volatile struct value       *value; /* reference to last value read */
    volatile uint32_t           in_use; /* atomic */
    thread_safe_var             vp;     /* for cleanup from thread key dtor */
    /* We could add a pthread_t here */
};

/*
 * Slots are allocated in arrays that are linked into one larger logical
 * array.
 */
struct slots {
    volatile struct slots   *next;      /* atomic */
    struct slot             *slot_array;/* atomic */
    volatile uint32_t       slot_count; /* atomic */
    uint32_t                slot_base;  /* logical index of slot_array[0] */
};

struct thread_safe_var_s {
    pthread_key_t           tkey;           /* to detect thread exits */
    pthread_mutex_t         write_lock;     /* one writer at a time */
    pthread_mutex_t         waiter_lock;    /* to signal waiters */
    pthread_cond_t          waiter_cv;      /* to signal waiters */
    var_dtor_t              dtor;           /* value destructor */
    volatile struct value   *values;        /* atomic ref'd value list head */
    volatile struct slots   *slots;         /* atomic reader subscription slots */
    volatile uint32_t       next_slot_idx;  /* atomic index of next new slot */
    volatile uint32_t       slots_in_use;   /* atomic count of live readers */
    uint32_t                nvalues;        /* writer-only; for housekeeping */
};

/*
 * Lock-less utility that scans through logical slot array looking for a
 * free slot to reuse.
 */
static struct slot *
get_free_slot(thread_safe_var vp)
{
    struct slots *slots;
    struct slot *slot;
    size_t i;

    for (slots = atomic_read_ptr((volatile void **)&vp->slots);
         slots != NULL;
         slots = atomic_read_ptr((volatile void **)&slots->next)) {
        for (i = 0; i < slots->slot_count; i++) {
            slot = &slots->slot_array[i];
            if (atomic_cas_32(&slot->in_use, 0, 1) == 0)
                return slot;
        }
    }
    return NULL;
}

/* Lock-less utility to get nth slot */
static struct slot *
get_slot(thread_safe_var vp, uint32_t slot_idx)
{
    struct slots *slots;
    uint32_t nslots = 0;

    for (slots = atomic_read_ptr((volatile void **)&vp->slots);
         slots != NULL;
         slots = atomic_read_ptr((volatile void **)&slots->next)) {
        nslots += slots->slot_count;
        if (nslots > slot_idx)
            break;
    }

    if (nslots <= slot_idx)
        return NULL;

    assert(slot_idx - slots->slot_base < slots->slot_count);
    return &slots->slot_array[slot_idx - slots->slot_base];
}

/* Lock-less utility to grow the logical slot array */
static int
grow_slots(thread_safe_var vp, uint32_t slot_idx, int tries)
{
    uint32_t nslots = 0;
    uint32_t additions;
    uint32_t i;
    volatile struct slots **slotsp;
    struct slots *new_slots;

    for (slotsp = &vp->slots;
         atomic_read_ptr((volatile void **)slotsp) != NULL;
         slotsp = &((struct slots *)atomic_read_ptr((volatile void **)slotsp))->next)
        nslots += (*slotsp)->slot_count;

    if (nslots > slot_idx)
        return 0;

    if (tries < 1)
        return EAGAIN; /* shouldn't happen; XXX assert? */

    if ((new_slots = calloc(1, sizeof(*new_slots))) == NULL)
        return errno;

    additions = (nslots == 0) ? 4 : nslots + nslots / 2;
    while (slot_idx >= nslots + additions) {
        /*
         * In this case we're racing with other readers to grow the slot
         * list, but if we lose the race then our slot_idx may not be
         * covered in the list as grown by the winner.  We may have to
         * try again.
         *
         * There's always a winner, so eventually we won't need to try
         * again.
         */
        additions += additions + additions / 2;
        tries++;
    }
    assert(slot_idx - nslots < additions);

    new_slots->slot_array = calloc(additions, sizeof(*new_slots->slot_array));
    if (new_slots->slot_array == NULL) {
        free(new_slots);
        return errno;
    }
    new_slots->slot_count = additions;
    new_slots->slot_base = nslots;
    for (i = 0; i < additions; i++) {
        new_slots->slot_array[i].in_use = 0;
        new_slots->slot_array[i].value = 0;
        new_slots->slot_array[i].vp = vp;
    }

    /* Reserve the slot we wanted (new slots not added yet) */
    atomic_write_32(&new_slots->slot_array[slot_idx - nslots].in_use, 1);

    /* Add new slots to logical array of slots */
    if (atomic_cas_ptr((volatile void **)slotsp, NULL, new_slots) != NULL) {
        /*
         * We lost the race to grow the array.  The index we wanted is
         * not guaranteed to be covered by the array as grown by the
         * winner.  We fall through to recurse to repeat.
         *
         * See commentary above where tries is incremented.
         */
        free(new_slots->slot_array);
        free(new_slots);
    }

    /*
     * If we won the race to grow the array then the index we wanted is
     * guaranteed to be present and recursing here is cheap.  If we lost
     * the race we need to retry.  We could goto the top of the function
     * though, just in case there's no tail call optimization.
     */
    return grow_slots(vp, slot_idx, tries - 1);
}

/* Utility to destroy a thread-safe global variable */
static void
destroy_var(thread_safe_var vp)
{
    struct slots *slots;
    struct value *val;

    if (vp == 0)
        return;

    pthread_mutex_lock(&vp->write_lock);

    while (vp->values != NULL) {
        val = atomic_read_ptr((volatile void **)&vp->values);
        vp->values = val->next;
        if (vp->dtor != NULL)
            vp->dtor(val->value);
        free(val);
    }
    while (vp->slots != NULL) {
        slots = atomic_read_ptr((volatile void **)&vp->slots);
        vp->slots = slots->next;
        free(slots->slot_array);
        free(slots);
    }
    vp->dtor = NULL;

    pthread_mutex_unlock(&vp->write_lock);
    pthread_mutex_destroy(&vp->write_lock);
    pthread_mutex_destroy(&vp->waiter_lock);
    pthread_cond_destroy(&vp->waiter_cv);
    /* XXX We leak var->tkey! */
}

/* Thread specific key destructor for handling thread exit */
void
release_slot(void *data)
{
    struct slot *slot = data;

    if (slot == NULL)
        return;

    /* Release value */
    atomic_write_ptr((volatile void **)&slot->value, NULL);

    /* Release slot */
    atomic_write_32(&slot->in_use, 0);

    /*
     * If the thread-safe global was destroyed while we held the last
     * slot then it falls to us to complete the destruction.
     */
    if (atomic_dec_32_nv(&slot->vp->slots_in_use) == 0)
        destroy_var(slot->vp);
}

/**
 * Initialize a thread-safe global variable
 *
 * A thread-safe global variable stores a current value, a pointer to
 * void, which may be set and read.  A value read from a thread-safe
 * global variable will be valid in the thread that read it, and will
 * remain valid until released or until the thread-safe global variable
 * is read again in the same thread.  New values may be set.  Values
 * will be destroyed with the destructor provided when no references
 * remain.
 *
 * @param var Pointer to thread-safe global variable
 * @param dtor Pointer to thread-safe global value destructor function
 *
 * @return Returns zero on success, else a system error number
 */
int
thread_safe_var_init(thread_safe_var *vpp,
                     thread_safe_var_dtor_f dtor)
{
    thread_safe_var vp;
    int err;

    *vpp = NULL;
    if ((vp = calloc(1, sizeof(*vp))) == NULL)
        return errno;

    vp->values = NULL;
    vp->slots = NULL;
    vp->dtor = dtor;
    vp->slots_in_use = 1; /* decremented upon destruction */
    vp->nvalues = 0;

    if ((err = pthread_key_create(&vp->tkey, release_slot)) != 0) {
        memset(vp, 0, sizeof(*vp));
        return err;
    }
    if ((err = pthread_mutex_init(&vp->write_lock, NULL)) != 0) {
        memset(vp, 0, sizeof(*vp));
        return err;
    }
    if ((err = pthread_mutex_init(&vp->waiter_lock, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        memset(vp, 0, sizeof(*vp));
        return err;
    }
    if ((err = pthread_cond_init(&vp->waiter_cv, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        pthread_mutex_destroy(&vp->waiter_lock);
        memset(vp, 0, sizeof(*vp));
        return err;
    }

    if ((err = grow_slots(vp, 3, 1)) != 0) {
        thread_safe_var_destroy(vp);
        return err;
    }

    assert(get_slot(vp, 0) != NULL);

    /*
     * Acquiring and dropping the lock functions as a trivial memory
     * barrier.
     */
    pthread_mutex_lock(&vp->write_lock);
    *vpp = vp;
    pthread_mutex_unlock(&vp->write_lock);
    *vpp = vp;
    return 0;
}

/**
 * Destroy a thread-safe global variable
 *
 * It is the caller's responsibility to ensure that no thread is using
 * this var and that none will use it again.
 *
 * @param [in] var The thread-safe global variable to destroy
 */
void
thread_safe_var_destroy(thread_safe_var vp)
{
    if (vp == 0)
        return;
    if (atomic_dec_32_nv(&vp->slots_in_use) > 0)
        return;     /* defer to last reader slot release via thread key dtor */
    destroy_var(vp);/* we're the last, destroy now */
}

/**
 * Get the most up to date value of the given cf var.
 *
 * @param [in] var Pointer to a cf var
 * @param [out] res Pointer to location where the variable's value will be output
 * @param [out] version Pointer (may be NULL) to 64-bit integer where the current version will be output
 *
 * @return Zero on success, a system error code otherwise
 */
int
thread_safe_var_get(thread_safe_var vp, void **res, uint64_t *version)
{
    int err = 0;
    uint32_t slot_idx;
    uint32_t slots_in_use;
    uint64_t vers;
    struct slot *slot;
    struct value *newest;

    if (version == NULL)
        version = &vers;
    *version = 0;
    *res = NULL;

    if ((slot = pthread_getspecific(vp->tkey)) == NULL) {
        /* First time for this thread -> O(N) slow path (subscribe thread) */
        slot_idx = atomic_inc_32_nv(&vp->next_slot_idx) - 1;
        if ((slot = get_free_slot(vp)) == NULL) {
            /* Slower path still: grow slots array list */
            err = grow_slots(vp, slot_idx, 2);  /* O(log N) */
            assert(err == 0);
            slot = get_slot(vp, slot_idx);      /* O(N) */
            assert(slot != NULL);
            atomic_write_32(&slot->in_use, 1);
        }
        assert(slot->vp == vp);
        slots_in_use = atomic_inc_32_nv(&vp->slots_in_use);
        assert(slots_in_use > 1);
        if ((err = pthread_setspecific(vp->tkey, slot)) != 0)
            return err;
    }

    /*
     * Else/then fast path: one acquire read, one release write, no
     * free()s.  O(1).
     *
     * We have to loop because we could read one value in the
     * conditional and that value could get freed if a writer runs
     * between the read in the conditional and the assignment to
     * slot->value with no other readers also succeeding in capturing
     * that value before that writer completes.
     *
     * This loop will run just once if there are no writers, and will
     * run as many times as writers can run between the conditional and
     * the body.  This loop can only be an infinite loop if there's an
     * infinite number of writers who run with higher priority than this
     * thread.  This is why writers sched_yield() before dropping their
     * write lock.
     *
     * Note that in the body of this loop we can write a soon-to-become-
     * invalid value to our slot because many writers can write between
     * the loop condition and the body.  The writer has to jump through
     * some hoops to deal with this.
     */
    while (atomic_read_ptr((volatile void **)&slot->value) !=
           (newest = atomic_read_ptr((volatile void **)&vp->values)))
        atomic_write_ptr((volatile void **)&slot->value, newest);

    if (newest != NULL) {
        *res = newest->value;
        *version = newest->version;
    }

    return 0;
}

/**
 * Release this thread's reference (if it holds one) to the current
 * value of the given thread-safe global variable.
 *
 * @param vp [in] A thread-safe global variable
 */
void
thread_safe_var_release(thread_safe_var vp)
{
    struct slot *slot;

    /* Always fast; never free()s.  O(1) */
    if ((slot = pthread_getspecific(vp->tkey)) == NULL)
        return;
    atomic_write_ptr((volatile void **)&slot->value, NULL);
    atomic_write_32(&slot->in_use, 0);
}

static volatile struct value *mark_values(thread_safe_var);

/**
 * Set new data on a thread-safe global variable
 *
 * @param [in] var Pointer to thread-safe global variable
 * @param [in] cfdata New value for the thread-safe global variable
 * @param [out] new_version New version number
 *
 * @return 0 on success, or a system error such as ENOMEM.
 */
int
thread_safe_var_set(thread_safe_var vp, void *data,
                    uint64_t *new_version)
{
    struct value *new_value;
    volatile struct value *old_values = NULL;
    volatile struct value *value;
    uint64_t vers;
    int err;

    if (new_version == NULL)
        new_version = &vers;
    *new_version = 0;

    if ((new_value = calloc(1, sizeof(*new_value))) == NULL)
        return errno;

    if ((err = pthread_mutex_lock(&vp->write_lock)) != 0) {
        free(new_value);
        return err;
    }

    /*
     * No allocations/free()s done with write lock held -> higher write
     * throughput.
     */

    new_value->value = data;
    new_value->next = atomic_read_ptr((volatile void **)&vp->values);
    if (new_value->next == NULL)
        new_value->version = 1;
    else
        new_value->version = new_value->next->version + 1;

    *new_version = new_value->version;

    /* Publish the new value */
    atomic_write_ptr((volatile void **)&vp->values, new_value);
    vp->nvalues++;

    if (*new_version < 2) {
        /* Signal waiters */
        (void) pthread_mutex_lock(&vp->waiter_lock);
        (void) pthread_cond_signal(&vp->waiter_cv); /* no thundering herd */
        (void) pthread_mutex_unlock(&vp->waiter_lock);
    }

    /* Now comes the slow part: garbage collect vp->values */
    old_values = mark_values(vp);

    /*
     * Because readers must loop, and could be kept from reading by a
     * long sequence of back-to-back higher-priority writers (presumably
     * all threads of a process will run with the same priority, but we
     * don't know that here), we yield the CPU before releasing the
     * write lock.  Hopefully we yield to a reader.
     */
    sched_yield();
    err = pthread_mutex_unlock(&vp->write_lock);

    /* Free old values now, holding no locks */
    for (value = old_values; value != NULL; value = old_values) {
        if (vp->dtor)
            vp->dtor(value->value);
        old_values = value->next;
        free((void *)value);
    }
    return err;
}

int
value_cmp(const void *a, const void *b)
{
    if (*(const struct value **)a < *(const struct value **)b)
        return -1;
    if (*(const struct value **)a > *(const struct value **)b)
        return 1;
    return 0;
}

volatile struct value *
value_binary_search(volatile struct value **seen, size_t n, volatile struct value *v)
{
    size_t left = 0;

    while (n > left) {
        size_t mid;

        /* Two or more array elements */
        mid = (left + n - 1) >> 1;
        if (seen[mid] < v)
            left = mid + 1; /* search right */
        else if (seen[mid] > v)
            n = mid;        /* search left */
        else
            return v; /* seen[mid] == v -> so we found v */
    }
    return NULL;
}

/* Mark half of mark-and-sweep GC */
static volatile struct value *
mark_values(thread_safe_var vp)
{
    volatile struct value **old_values_array;
    volatile struct value * volatile *p;
    volatile struct value *old_values = NULL;
    volatile struct value *v, *v2;
    volatile struct slots *slots;
    struct slot *slot;
    size_t i;

    old_values_array = calloc(vp->nvalues, sizeof(old_values_array[0]));

    /*
     * XXX There should be no need to atomically read vp->values here,
     * as we are the writer and hold a lock.
     */
    for (i = 0, v = atomic_read_ptr((volatile void **)&vp->values);
         i < vp->nvalues && v != NULL;
         v = v->next, i++)
        old_values_array[i] = v;
    assert(i == vp->nvalues && v == NULL);
    qsort(old_values_array, vp->nvalues, sizeof(old_values_array[0]),
          value_cmp);
    /* Assert that qsort() sorted */
    for (i = 1; i < vp->nvalues; i++)
        assert(old_values_array[i-1] < old_values_array[i]);

    /*
     * Mark. This is O(N log(M)) where N is the number of subscribed
     * threads and M is the number of values, but with the optimizations
     * below, and with a bit of luck, this is more like O(N) than like
     * O(N log(M)).
     */
    vp->values->referenced = 1; /* curr value is always in use */

    for (i = 0, slots = atomic_read_ptr((volatile void **)&vp->slots);
         slots != NULL;
         i++) {
        assert(slots != NULL);
        assert(i >= slots->slot_base);
        assert(i <= slots->slot_base + slots->slot_count);
        if (i == slots->slot_base + slots->slot_count) {
            slots = slots->next;
            if (slots == NULL)
                break;
        }
        assert(slots->slot_count > 0);
        assert(i >= slots->slot_base);
        assert(i < slots->slot_base + slots->slot_count);
        slot = &slots->slot_array[i - slots->slot_base];
        v = atomic_read_ptr((volatile void **)&slot->value);

        /*
         * Optimization: ignore slots with a NULL value.  The owner of
         * that slot may be about to write a value that we're about to
         * free, but they will notice that multiple writers went by and
         * re-read vp->value.
         *
         * Also ignore slots with the current value.
         */
        if (v == NULL || v == vp->values)
            continue;

        /*
         * We can't just dereference v->referenced because there's a
         * window in the get-side where we can set the slot's value to
         * an immediately-after free()'ed value, and we could be seeing
         * such a value, which means we can't dereference it.
         *
         * Instead we search for v in the old_values_array[].  If it's
         * found then it's safe to write to v->referenced because it is
         * stable through the execution of this function and won't be
         * free()'ed until after.
         */
        if ((value_binary_search(old_values_array,
                                 vp->nvalues, v)) != NULL) {
            v->referenced = 1;  /* so v is valid, safe to deref */
            continue;
        }

#ifndef NDEBUG
        for (v2 = vp->values; v2 != NULL; v2 = v2->next)
            assert(v2 != v);
#endif
    }
    free(old_values_array);

    /* Sweep; O(N) where N is the number of referenced values */
    for (p = &vp->values; *p != NULL;) {
        v = *p;

        if (!v->referenced) {
            assert(v != vp->values);

            /* Remove from list and setup to continue at v->next */
            *p = v->next;
            /* Prepend v to old_values list */
            v->next = old_values;
            old_values = v;
            vp->nvalues--;

            /* Sweep the remainder of the list */
            continue;
        }

        /* Step into this value's next sub-list */
        v->referenced = 0;
        p = &v->next;
        assert(p != &vp->values);
    }

    errno = 0;
    return old_values;
}

#endif /* USE_TSV_SLOT_PAIR_DESIGN */

/* Code common to both implementations */

/**
 * Wait for a var to have its first value set.
 *
 * @param vp [in] The vp to wait for
 *
 * @return Zero on success, else a system error
 */
int
thread_safe_var_wait(thread_safe_var vp)
{
    void *junk;
    int err;

    if ((err = thread_safe_var_get(vp, &junk, NULL)) == 0 && junk != NULL)
        return 0;

    if ((err = pthread_mutex_lock(&vp->waiter_lock)) != 0)
        return err;

    while ((err = thread_safe_var_get(vp, &junk, NULL)) == 0 &&
           junk == NULL) {
        if ((err = pthread_cond_wait(&vp->waiter_cv,
                                     &vp->waiter_lock)) != 0) {
            (void) pthread_mutex_unlock(&vp->waiter_lock);
            return err;
        }
    }

    /*
     * The first writer signals, rather than broadcast, to avoid a
     * thundering herd.  We propagate the signal here so the rest of the
     * herd wakes, one at a time.
     */
    (void) pthread_cond_signal(&vp->waiter_cv);
    if ((err = pthread_mutex_unlock(&vp->waiter_lock)) != 0)
        return err;
    return err;
}

#if 0
/*
 * Common code.
 */

struct desc_table {
    
};

static volatile uint32_t next_var_idx;

/* Append-only rope of TSVs */
struct thread_safe_vars {
    struct thread_safe_vars *next;
    thread_safe_var **vars;
    uint32_t nvars;
};
static struct thread_safe_vars vars;

static thread_safe_var *
get_var(uint32_t idx)
{
    struct thread_safe_vars *vs;

    for (vs = &vars; vs; vs = atomic_read_ptr((volatile void **)&vs->next)) {
        uint32_t nvars = atomic_read_32(&vs->nvars);
        thread_safe_var *vsvars = atomic_read_ptr((volatile void **)&vs->vars);

        if (idx < nvars)
            return &vsvars[idx];
        idx -= nvars;
    }
    return NULL;
}

static thread_safe_var *
new_var(uint32_t *idxp)
{
    struct thread_safe_vars *vs, *nvs;
    thread_safe_var *slot;
    uint32_t idx, nvars;

    *idxp = idx = atomic_inc_32_nv(&next_var_idx);
    if ((slot = get_var(idx)))
        return slot;

    /* Allocate the next vars rope segment */
    nvs = calloc(1, sizeof(*nvs));
    if (nvs == NULL)
        return NULL;

    for (vs = &vars; ; vs = atomic_read_ptr((volatile void **)&vs->next)) {
        nvars = atomic_read_32(&vs->nvars);

        /* Size the next vars rope segment */
        nvars = nvars + (nvars >> 1) + 4;
        if (idx >= nvars)
            nvars = idx + 4;
        atomic_write_32(&nvs->nvars, nvars);

        /* Install the next vars rope segment */
        if (atomic_cas_ptr((volatile void **)&vs->next, NULL, (void *)nvs) == NULL)
            nvs = NULL; /* This thread's new segment won! */

        /*
         * Re-read nvars for this new segment as a different thread
         * might have computed a different value due to the
         *
         *   if (idx >= nvars) nvars = idx + 4;
         *
         * above.
         */
        nvars = atomic_read_32(&vs->nvars);
        if (idx < nvars) {
            thread_safe_var *vars = atomic_read_ptr((volatile void **)&vs->vars);
            thread_safe_var *found;

            /* Race to install the `vars` array in the new rope segment */
            if (vars == NULL) {
                vars = calloc(nvars, sizeof(vs->vars[0]));
                if (vars == NULL) {
                    free(nvs);
                    return NULL;
                }
                found = atomic_cas_ptr((volatile void **)&vs->vars, NULL, (void *)vars);
                if (found) {
                    free(vars);
                    vars = found;
                }
            }
            return &vars[idx];
        }

        /*
         * Lost the race, but we might still have to install a new
         * segment.
         */
        idx -= nvars;
    }
}

static pthread_key_t tkey;
static void once(void)
{
}
#endif
