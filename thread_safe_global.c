
/*
 * This implements a thread-safe variable.  A thread can read it, and the
 * value it reads will be safe to use until it reads it again.
 *
 * Properties:
 *
 *  - writers are serialized
 *  - readers are fast, rarely doing blocking operations, and when they
 *    do, not blocking on contended resources
 *  - readers do not starve writers; writers do not block readers
 *
 * The design for this implementation uses a pair of slots such that one
 * has a current value for the variable, and the other holds the
 * previous value and will hold the next value.
 */
/*
 * Alternative Design
 *
 * An alternative design could have a linked list where the head and the
 * next pointers are the only things written to by the writer, and where
 * all readers "subscribe" and thence their state consists of a single
 * pointer to what was at read-time the head of that linked list, with
 * that pointer held where the writers can find it (in "subscription"
 * slots) so they can garbage collect in order to release no-longer
 * referenced values.
 *
 * Subscription can be lock-less.  There'd be a index into a logical
 * array of subscription slots.  New reader threads would atomically
 * increment a counter to determine their index into this array.  If the
 * array index goes past the allocated array size, then the array would
 * be grown.  The array could be maintained as a linked list of array
 * chunks or as a tree of array chunks, either way, when a reader goes
 * to grow it, it will either win the race or lose it, using an atomic
 * CAS operation to perform the growth; losers free their chunk and then
 * find their slot in the winner's chunk.
 *
 * Once subcribed, readers only ever do an acquire-fenced read on the
 * head of the linked list of values, and write that to their slot with
 * a release-fenced write.
 *
 * Writers only add new values at the head of the list, with the
 * previous head as the next pointer of the new element.
 *
 * Writers also mark-and-sweep garbage collect the extant list by
 * reading every subscribed thread's pointer with an acquire-fenced
 * read, marking all in-use values as such, then the writer releases and
 * removes from the list those elements not marked as in-used.  Readers
 * never read the next pointers of the list's elements.
 *
 * Readers do two fenced memory operations.  Writers do N fenced memory
 * operations plus the writer lock acquire/release and any locks
 * required to allocate and free list elements.  Readers may have to
 * allocate the first time they read, but not thereafter.  No
 * thread-specific is needed.
 */
/*
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
 *            We could probably make this code more efficient using
 *            explicit acquire/release membars and fewer CASes, on some
 *            CPUs anyways.  But this should already be quite a lot
 *            faster for readers than using read-write locks, and a much
 *            more convenient and natural API to boot.
 *
 *            In any case: first make it work, then optimize.
 */

/*
 * TODO:
 *
 *  - Use a single global thread-specific key, not a per-variable one.
 *
 *    This is important as otherwise we leak thread-specific keys.
 *
 *  - Add a getter that gets the value saved in a thread-local, rather
 *    than a fresh value.  Or make this an argument to the get function.
 *
 *  - Rename / add option to rename all the symbols to avoid using the
 *    pthread namespace.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "thread_safe_global.h"
#include "atomics.h"

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
pthread_var_init_np(pthread_var_np_t *vp,
                      pthread_var_destructor_np_t dtor)
{
    int err;

    memset(vp, 0, sizeof(*vp));

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
    if ((err = pthread_mutex_init(&vp->cv_lock, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        pthread_mutex_destroy(&vp->cv_lock);
        memset(vp, 0, sizeof(*vp));
        return err;
    }
    if ((err = pthread_cond_init(&vp->cv, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        pthread_mutex_destroy(&vp->waiter_lock);
        pthread_mutex_destroy(&vp->cv_lock);
        memset(vp, 0, sizeof(*vp));
        return err;
    }
    if ((err = pthread_cond_init(&vp->waiter_cv, NULL)) != 0) {
        pthread_mutex_destroy(&vp->write_lock);
        pthread_mutex_destroy(&vp->waiter_lock);
        pthread_mutex_destroy(&vp->cv_lock);
        pthread_cond_destroy(&vp->cv);
        memset(vp, 0, sizeof(*vp));
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
pthread_var_destroy_np(pthread_var_np_t *vp)
{
    if (vp == 0)
        return;

    pthread_var_release_np(vp);
    pthread_mutex_lock(&vp->write_lock); /* There'd better not be readers */
    pthread_cond_destroy(&vp->cv);
    pthread_mutex_destroy(&vp->cv_lock);
    wrapper_free(vp->vars[0].wrapper);
    wrapper_free(vp->vars[1].wrapper);
    memset(vp, 0, sizeof(*vp));
    vp->vars[0].other = &vp->vars[1];
    vp->vars[1].other = &vp->vars[0];
    vp->vars[0].wrapper = NULL;
    vp->vars[1].wrapper = NULL;
    vp->dtor = NULL;
    pthread_mutex_unlock(&vp->write_lock);
    pthread_mutex_destroy(&vp->write_lock);
    /* Remaining references will be released by the thread key destructor */
    /* XXX We leak var->tkey!  See note in initiator above. */
}

static int
signal_writer(pthread_var_np_t *vp)
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
int pthread_var_init_np(pthread_var_np_t *, pthread_var_destructor_np_t);
void pthread_var_destroy_np(pthread_var_np_t *);
int pthread_var_get_np(pthread_var_np_t *, void **, uint64_t *);
int
pthread_var_get_np(pthread_var_np_t *vp, void **res, uint64_t *version)
{
    int err = 0;
    int err2 = 0;
    int err3 = 0;
    size_t i;
    uint32_t nref;
    struct var *v;
    uint64_t vers, vers2;
    struct vwrapper *wrapper;
    int got_both_slots = 0; /* Whether we incremented both slots' nreaders */
    int do_signal_writer = 0;

    if (version == NULL)
        version = &vers;
    *version = 0;

    *res = NULL;

#ifndef NO_FAST_PATH
    /*
     * This is #ifndef'ed to make it possible to test without the fast
     * path: it might make it easier to trip race conditions that might
     * not be handled below.
     */
    {
        wrapper = pthread_getspecific(vp->tkey);
        if (wrapper != NULL &&
            wrapper->version == 1 + atomic_cas_64(&vp->next_version, 0, 0)) {
            *version = 1 + wrapper->version;
            *res = wrapper->ptr;
            return 0;
        }
    }
#endif

    /* Get the current next version */
    *version = atomic_cas_64(&vp->next_version, 0, 0);
    if (*version == 0) {
        /* Not set yet */
        assert(*version == 0 || *res != NULL);
        return 0;
    }
    (*version)--; /* make it the current version */

    /* Get what we hope is still the current slot */
    v = &vp->vars[(*version) & 0x1];

    /*
     * We picked a slot, but we could just have lost against one or more
     * writers.  So far nothing we've done would block any number of
     * them.
     *
     * We increment nreaders for the slot we picked to keep out
     * subsequent writers; we can then lose one more race at most.
     *
     * But we still need to learn whether we lost the race.
     */
    (void) atomic_inc_32_nv(&v->nreaders);

    /* See if we won any race */
    if ((vers2 = atomic_cas_64(&vp->next_version, 0, 0)) == *version) {
        /*
         * We won, or didn't race at all.  We can now safely
         * increment nref for the wrapped value in the current slot.
         *
         * We can still have lost one race, but this slot is now ours.
         *
         * The key here is that we updated nreaders for one slot,
         * which might not keep the one writer we might have been
         * racing with from making the then current slot the now
         * previous slot, but because writers are serialized it will
         * keep the next writer from touching the slot we thought
         * was the current slot.  Thus here we either have the
         * current slot or the previous slot, and either way it's OK
         * for us to grab a reference to the wrapped value in the
         * slot we took.
         */
        goto got_a_slot;
    }

    /*
     * We may have incremented nreaders for the wrong slot.  Any number
     * of writers could have written between our getting
     * vp->next_version the first time, and our incrementing nreaders
     * for the corresponding slot.  We can't incref the nref of the
     * value wrapper found at the slot we picked.  We first have to find
     * the correct current slot, or ensure that no writer will release
     * the other slot.
     *
     * We increment the reader count on the other slot, but we do it
     * *before* decrementing the reader count on this one.  This should
     * guarantee that we find the other one present by keeping
     * subsequent writers (subsequent to the second writer we might be
     * racing with) out of both slots for the time between the update of
     * one slot's nreaders and the other's.
     *
     * We then have to repeat the race loss detection.  We need only do
     * this at most once.
     */
    atomic_inc_32_nv(&v->other->nreaders);

    /* We hold both slots */
    got_both_slots = 1;

    /*
     * vp->next_version can now increment by at most one, and we're
     * guaranteed to have one usable slot (whichever one we _now_ see as
     * the current slot, and which can still become the previous slot).
     */
    vers2 = atomic_cas_64(&vp->next_version, 0, 0);
    assert(vers2 > *version);
    *version = vers2 - 1;

    /* Select a slot that looks current in this thread */
    v = &vp->vars[(*version) & 0x1];

got_a_slot:
    if (v->wrapper == NULL) {
        /* Whoa, nothing there; shouldn't happen; assert? */
        assert(*version == 0);
        assert(*version == 0 || *res != NULL);
        if (got_both_slots && atomic_dec_32_nv(&v->other->nreaders) == 0) {
            /* Last reader of a slot -> signal writer. */
            do_signal_writer = 1;
        }
        if (atomic_dec_32_nv(&v->nreaders) == 0 || do_signal_writer)
            err2 = signal_writer(vp);
        return (err2 == 0) ? err : err2;
    }

    assert(vers2 == atomic_cas_64(&vp->next_version, 0, 0) ||
           (vers2 + 1) == atomic_cas_64(&vp->next_version, 0, 0));

    /* Take the wrapped value for the slot we chose */
    nref = atomic_inc_32_nv(&v->wrapper->nref);
    assert(nref > 1);
    *version = v->wrapper->version;
    *res = atomic_cas_ptr((volatile void **)&v->wrapper->ptr, NULL, NULL);
    assert(*res != NULL);

    /*
     * We'll release the previous wrapper and save the new one in
     * vp->tkey below, after releasing the slot it came from.
     */
    wrapper = v->wrapper;

    /*
     * Release the slot(s) and signal any possible waiting writer if
     * either slot's nreaders drops to zero (that's what the writer will
     * be waiting for).
     *
     * The one blocking operation done by readers happens in
     * signal_writer(), but that one blocking operation is for a lock
     * that the writer will have or will soon have released, so it's
     * a practically uncontended blocking operation.
     */
    if (got_both_slots && atomic_dec_32_nv(&v->other->nreaders) == 0)
        do_signal_writer = 1;
    if (atomic_dec_32_nv(&v->nreaders) == 0 || do_signal_writer)
        err2 = signal_writer(vp);

    /*
     * Release the value previously read in this thread, if any.
     *
     * Note that we call free() here, which means that we can take a
     * lock.  The application's value destructor also can do the same.
     *
     * TODO/FIXME We could use a lock-less queue/stack to queue up
     *            wrappers for destruction by writers, then readers
     *            could be even more light-weight.
     */
    if (*res != pthread_getspecific(vp->tkey))
        pthread_var_release_np(vp);

    /* Recall this value we just read */
    err = pthread_setspecific(vp->tkey, wrapper);
    return (err2 == 0) ? err : err2;
}

/**
 * Wait for a var to have its first value set.
 *
 * @param vp [in] The vp to wait for
 *
 * @return Zero on success, else a system error
 */
int
pthread_var_wait_np(pthread_var_np_t *vp)
{
    void *junk;
    int err;

    if ((err = pthread_var_get_np(vp, &junk, NULL)) == 0 && junk != NULL)
        return 0;

    if ((err = pthread_mutex_lock(&vp->waiter_lock)) != 0)
        return err;

    while ((err = pthread_var_get_np(vp, &junk, NULL)) == 0 &&
           junk == NULL) {
        if ((err = pthread_cond_wait(&vp->waiter_cv,
                                     &vp->waiter_lock)) != 0) {
            (void) pthread_mutex_unlock(&vp->waiter_lock);
            return err;
        }
    }

    /*
     * The first writer signals, rather than broadcase, to avoid a
     * thundering herd.  We propagate the signal here so the rest of the
     * herd wakes, one at a time.
     */
    (void) pthread_cond_signal(&vp->waiter_cv);
    if ((err = pthread_mutex_unlock(&vp->waiter_lock)) != 0)
        return err;
    return err;
}

/**
 * Release this thread's reference (if it holds one) to the current
 * value of the given thread-safe global variable.
 *
 * @param vp [in] A thread-safe global variable
 */
void
pthread_var_release_np(pthread_var_np_t *vp)
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
pthread_var_set_np(pthread_var_np_t *vp, void *cfdata,
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

    if ((err = pthread_mutex_lock(&vp->write_lock)) != 0) {
        free(wrapper);
        return err;
    }

    /* vp->next_version is stable because we hold the write_lock */
    *new_version = wrapper->version = atomic_cas_64(&vp->next_version, 0, 0);

    /* Grab the next slot */
    v = vp->vars[(*new_version + 1) & 0x1].other;
    old_wrapper = atomic_cas_ptr((volatile void **)&v->wrapper, NULL, NULL);

    if (*new_version == 0) {
        /* This is the first write; set wrapper on both slots */

        for (i = 0; i < sizeof(vp->vars)/sizeof(vp->vars[0]); i++) {
            v = &vp->vars[i];
            nref = atomic_inc_32_nv(&wrapper->nref);
            v->version = 0;
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

    assert(old_wrapper != NULL && old_wrapper->nref > 0);

    /* Wait until that slot is quiescent before mutating it */
    if ((err = pthread_mutex_lock(&vp->cv_lock)) != 0) {
        (void) pthread_mutex_unlock(&vp->write_lock);
        free(wrapper);
        return err;
    }
    while (atomic_cas_32(&v->nreaders, 0, 0) > 0) {
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
    tmp_version = atomic_inc_64_nv(&vp->next_version);
    assert(tmp_version == *new_version + 1);
    assert(v->version > v->other->version);

    /* Release the old cf */
    assert(old_wrapper != NULL && old_wrapper->nref > 0);
    wrapper_free(old_wrapper);

    /* Done */
    return pthread_mutex_unlock(&vp->write_lock);
}
