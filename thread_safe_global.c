
/*
 * This implements a thread-safe variable.  A thread can read it, and the
 * value it reads will be safe to use until it reads it again.
 *
 * Properties:
 *
 *  - writers are serialized
 *  - readers are fast, rarely doing blocking operations, and when they
 *    do, not blocking on contended resources
 *  - readers do not starve writers
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
 * writer starvation: readers *never* block on contended resources.  We
 * achieve this by having two value slots: one for the current value,
 * and one for the previous/next value.  Readers can always lock-less-ly
 * find one of the two values.
 *
 * Whereas in the case of a read-write lock without writer starvation,
 * readers arriving after a writer must get held up for the writer who,
 * in turn, is held up by readers.  Therefore, for the typical case
 * where one uses read-write locks (to mediate access to rarely-changing
 * mostly-read-only "configuration"), the API implemented here is
 * superior to read-write locks.
 *
 * NOTE WELL: We assume that atomic operations imply memory barriers.
 *
 *            The general rule is that all things which are to be
 *            atomically modified in some cases are always modified
 *            atomically, except at initialization time, and even then,
 *            in one case the initialized value is immediately modified
 *            with an atomic operation.  This is to ensure memory
 *            visibility rules (see above).
 *
 *            We could probably make this code more efficient using
 *            explicit membars, on some CPUs anyways.  But this should
 *            already be quite a lot faster for readers than using
 *            read-write locks.
 */

/*
 * TODO:
 *
 *  - Add a getter that gets the value saved in a thread-local, rather
 *    than a fresh value.  Or make this an argument to the get function.
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "thread_safe_global.h"

#ifndef HAVE_ATOMICS
extern uint32_t atomic_inc_32_nv(volatile uint32_t *);
extern uint64_t atomic_inc_64_nv(volatile uint64_t *);
extern uint32_t atomic_dec_32_nv(volatile uint32_t *);
extern void *atomic_cas_ptr(volatile void *, void *, void *);
extern uint64_t atomic_cas_64(volatile uint64_t *, uint64_t, uint64_t);
#endif


static void
wrapper_free(struct cfwrapper *wrapper)
{
    if (wrapper == NULL || atomic_dec_32_nv(&wrapper->nref) != 0)
        return;
    if (wrapper->dtor != NULL)
        wrapper->dtor(wrapper);
    wrapper->wrapper_dtor(wrapper);
}

static void
cfvar_dtor_wrapper(void *wrapper)
{
    wrapper_free(wrapper);
}

/**
 * Initialize a configuration variable
 *
 * A configuration variable stores a current value, a pointer to void,
 * which may be set and read.  A value read from a configuration
 * variable will be valid in the thread that read it, and will remain
 * valid until released or until the configuration variable is read
 * again in the same thread.  New values may be set.  Values will be
 * destroyed with the destructor provided when no references remain.
 *
 * @param cfvar Pointer to configuration variable
 * @param dtor Pointer to configuration value destructor function
 *
 * @return Returns zero on success, else a system error number
 */
int
pthread_cfvar_init_np(pthread_cfvar_np_t *cfv,
                      pthread_cfvar_destructor_np_t dtor)
{
    int err;

    memset(cfv, 0, sizeof(*cfv));

    /*
     * The thread-local key is used to hold a reference for destruction
     * at thread-exit time, if the thread does not explicitly drop the
     * reference before then.
     *
     * There's no pthread_key_destroy(), so we leak these.  We could
     * have a single global thread key pointing to an array of keys.
     */
    if ((err = pthread_key_create(&cfv->tkey, cfvar_dtor_wrapper)) != 0) {
        memset(cfv, 0, sizeof(*cfv));
        return err;
    }
    if ((err = pthread_mutex_init(&cfv->write_lock, NULL)) != 0) {
        memset(cfv, 0, sizeof(*cfv));
        return err;
    }
    if ((err = pthread_mutex_init(&cfv->waiter_lock, NULL)) != 0) {
        pthread_mutex_destroy(&cfv->write_lock);
        memset(cfv, 0, sizeof(*cfv));
        return err;
    }
    if ((err = pthread_mutex_init(&cfv->cv_lock, NULL)) != 0) {
        pthread_mutex_destroy(&cfv->write_lock);
        pthread_mutex_destroy(&cfv->cv_lock);
        memset(cfv, 0, sizeof(*cfv));
        return err;
    }
    if ((err = pthread_cond_init(&cfv->cv, NULL)) != 0) {
        pthread_mutex_destroy(&cfv->write_lock);
        pthread_mutex_destroy(&cfv->waiter_lock);
        pthread_mutex_destroy(&cfv->cv_lock);
        memset(cfv, 0, sizeof(*cfv));
        return err;
    }
    if ((err = pthread_cond_init(&cfv->waiter_cv, NULL)) != 0) {
        pthread_mutex_destroy(&cfv->write_lock);
        pthread_mutex_destroy(&cfv->waiter_lock);
        pthread_mutex_destroy(&cfv->cv_lock);
        pthread_cond_destroy(&cfv->cv);
        memset(cfv, 0, sizeof(*cfv));
        return err;
    }

    /*
     * cfv->version is a 64-bit unsigned int.  If ever we can't get
     * atomics to deal with it on 32-bit platforms we could have a
     * pointer to one of two version numbers which are not atomically
     * updated, and instead atomically update the pointer.
     */
    cfv->version = 0;
    cfv->vars[0].nreaders = 0;
    cfv->vars[0].wrapped = NULL;
    cfv->vars[0].other = &cfv->vars[1]; /* other pointer never changes */
    cfv->vars[1].nreaders = 0;
    cfv->vars[1].wrapped = NULL;
    cfv->vars[1].other = &cfv->vars[0]; /* other pointer never changes */
    cfv->dtor = dtor;
    return 0;
}

/**
 * Destroy a configuration variable
 *
 * It is the caller's responsibility to ensure that no thread is using
 * this cfvar and that none will use it again.
 *
 * @param [in] cfvar The configuration variable to destroy
 */
void
pthread_cfvar_destroy_np(pthread_cfvar_np_t *cfvar)
{
    if (cfvar == 0)
        return;

    pthread_cond_destroy(&cfvar->cv);
    pthread_mutex_destroy(&cfvar->cv_lock);
    pthread_mutex_destroy(&cfvar->write_lock);
    /* CFs will be released by the thread-specifics' destructors */
    /* We leak cfvar->tkey!  See note in initiator above. */
}

/**
 * Get the most up to date value of the given cf var.
 *
 * @param [in] cfvar Pointer to a cf var
 * @param [out] res Pointer to location where configuration data will be output
 * @param [out] version Pointer (may be NULL) to 64-bit integer where the current version will be output
 *
 * @return Zero on success, a system error code otherwise
 */
int pthread_cfvar_init_np(pthread_cfvar_np_t *, pthread_cfvar_destructor_np_t);
void pthread_cfvar_destroy_np(pthread_cfvar_np_t *);
int pthread_cfvar_get_np(pthread_cfvar_np_t *, void **, uint64_t *);
int
pthread_cfvar_get_np(pthread_cfvar_np_t *cfv, void **res, uint64_t *version)
{
    int err;
    size_t i;
    uint32_t nref;
    struct cfvar *v;
    uint64_t vers, vers2;

    if (version == NULL)
        version = &vers;

    *res = NULL;

    /* Get the current slot */
    *version = atomic_cas_64(&cfv->version, 0, 0); /* racy; see below */
    if (*version == 0) {
        /* Not set yet */
        return 0;
    }
    v = &cfv->vars[(*version + 1) & 0x1];

    /*
     * We picked a slot, but we could just have lost against a writer.
     *
     * We increment nreaders for that slot to keep out subsequent
     * writers: we can lose this race at most once per turn.
     *
     * But we still need to learn whether we lost the race.
     */
    (void) atomic_inc_32_nv(&v->nreaders);
    for (i = 2; i > 0; i--) {
        if (v->wrapped == NULL) {
            /* Whoa, nothing there; shouldn't happen */

            (void) atomic_dec_32_nv(&v->nreaders);
            return 0;
        }

        /* See if we won any race */
        if (atomic_cas_64(&cfv->version, 0, 0) == *version) {
            /*
             * We won, or didn't race at all.  We can now safely
             * increment nref for the wrapped value in the current slot.
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
            nref = atomic_inc_32_nv(&v->wrapped->nref);
            assert(nref > 1);
            *version = v->wrapped->version;
            *res = v->wrapped->ptr;

            (void) atomic_dec_32_nv(&v->nreaders);
            pthread_cfvar_release_np(cfv);
            return pthread_setspecific(cfv->tkey, v->wrapped);
        }

        /*
         * We lost the race with at most one writer for that slot.  So
         * we now have a hold of a slot that isn't the current slot and
         * we might be racing with a second writer who will release the
         * value in the slot we do have a reference to.  If we tried to
         * incref this slot's value now, that would be a disaster.
         *
         * Instead we increment the reader count on the other slot, but
         * we do it *before* decrementing the reader count on this one.
         * This should guarantee that we find the other one present by
         * keeping subsequent writers (subsequent to the second writer
         * we might be racing with) out of both slots for the time
         * between the update of one slot's nreaders and the other's.
         *
         * We have to repeat the race loss detection for the second
         * slot, thus this is a loop.  But we need only do this at most
         * once.
         */
        atomic_inc_32_nv(&v->other->nreaders);
        vers2 = atomic_cas_64(&cfv->version, 0, 0);
        assert(vers2 == ++(*version));
        *version = vers2;
        if (atomic_dec_32_nv(&v->nreaders) == 0) {
            /*
             * As the last reader of this v we have to signal the
             * writer that may be waiting for us to stop reading.
             *
             * Errors here could be ignored, but leaving a writer stuck
             * is worth indicating.
             */
            if ((err = pthread_mutex_lock(&cfv->cv_lock)) != 0) {
                atomic_dec_32_nv(&v->other->nreaders);
                return err;
            }
            if ((err = pthread_cond_broadcast(&cfv->cv)) != 0) {
                atomic_dec_32_nv(&v->other->nreaders);
                (void) pthread_mutex_unlock(&cfv->cv_lock);
                return err;
            }
            if ((err = pthread_mutex_unlock(&cfv->cv_lock)) != 1) {
                atomic_dec_32_nv(&v->other->nreaders);
                return err;
            }
        }
        v = v->other;
        assert(v == &cfv->vars[(*version + 1) & 0x1]);
    }

    /* We shouldn't get here */
    assert(cfv->version == (uint64_t)-1);
    (void) atomic_dec_32_nv(&v->nreaders);
    return EAGAIN;
}

/**
 * Wait for a cfvar to have its first value set.
 *
 * @param cfv [in] The cfv to wait for
 *
 * @return Zero on success, else a system error
 */
int
pthread_cfvar_wait_np(pthread_cfvar_np_t *cfv)
{
    void *junk;
    int err;

    if ((err = pthread_mutex_lock(&cfv->waiter_lock)) != 0)
        return err;

    while ((err = pthread_cfvar_get_np(cfv, &junk, NULL)) == 0 &&
           junk == NULL) {
        if ((err = pthread_cond_wait(&cfv->waiter_cv,
                                     &cfv->waiter_lock)) != 0) {
            (void) atomic_dec_32_nv(&cfv->waiters);
            (void) pthread_mutex_unlock(&cfv->waiter_lock);
            return err;
        }
    }

    (void) atomic_dec_32_nv(&cfv->waiters);
    (void) pthread_cond_signal(&cfv->waiter_cv); /* no thundering herd */
    if ((err = pthread_mutex_unlock(&cfv->waiter_lock)) != 0)
        return err;
    return err;
}

/**
 * Release this thread's reference (if it holds one) to the current
 * value of the given configuration variable.
 *
 * @param cfv [in] A configuration variable
 */
void
pthread_cfvar_release_np(pthread_cfvar_np_t *cfv)
{
    struct cfwrapper *wrapper = pthread_getspecific(cfv->tkey);

    if (wrapper == NULL)
        return;
    if (pthread_setspecific(cfv->tkey, NULL) != 0 &&
        pthread_getspecific(cfv->tkey) == wrapper)
        return;
    wrapper_free(wrapper);
}

/**
 * Set new data on a configuration variable
 *
 * @param [in] cfvar Pointer to configuration variable
 * @param [in] cfdata Pointer to configuration data
 * @param [out] new_version New version number
 *
 * @return 0 on success, or a system error such as ENOMEM.
 */
int
pthread_cfvar_set_np(pthread_cfvar_np_t *cfv, void *cfdata,
                     uint64_t *new_version)
{
    int err;
    size_t i;
    struct cfvar *v;
    struct cfwrapper *old_wrapper = NULL;
    struct cfwrapper *wrapper;

    *new_version = 0;

    if ((wrapper = calloc(1, sizeof(*wrapper))) == NULL)
        return errno;

    /*
     * The cfvar itself holds a reference to the current value, thus its
     * nref starts at 1.
     */
    wrapper->wrapper_dtor = free;
    wrapper->dtor = cfv->dtor;
    wrapper->nref = 0;
    (void) atomic_inc_32_nv(&wrapper->nref);
    wrapper->ptr = cfdata;

    if ((err = pthread_mutex_lock(&cfv->write_lock)) != 0) {
        free(wrapper);
        return err;
    }

    *new_version = wrapper->version = (atomic_cas_64(&cfv->version, 0, 0) + 1);

    /* Grab the next slot (the previous one to the current one) */
    v = &cfv->vars[(*new_version + 1) & 0x1];
    old_wrapper = atomic_cas_ptr(&v->wrapped, NULL, NULL);

    if (*new_version == 1) {
        /* This is the first cf being written; easy */
        v = &cfv->vars[0];

        /* These writes can become visible out of order */
        (void) atomic_cas_ptr(&v->wrapped, old_wrapper, wrapper);
        (void) atomic_inc_64_nv(&cfv->version);
        (void) pthread_mutex_lock(&cfv->waiter_lock);
        (void) pthread_cond_signal(&cfv->waiter_cv); /* no thundering herd */
        (void) pthread_mutex_unlock(&cfv->waiter_lock);
        return pthread_mutex_unlock(&cfv->write_lock);
    }

    /* Wait until that slot is quiescent before mutating it */
    if ((err = pthread_mutex_lock(&cfv->cv_lock)) != 0) {
        (void) pthread_mutex_unlock(&cfv->write_lock);
        free(wrapper);
        return err;
    }
    while (atomic_cas_32(&v->nreaders, 0, 0) > 0) {
        /*
         * We have a separate lock for writing vs. waiting so that no
         * other writer can steal our march.  We got here by winning the
         * race for the writer lock, so we'll hold onto it, and thus
         * avoid having to restart here.
         */
        if ((err = pthread_cond_wait(&cfv->cv, &cfv->cv_lock)) != 0) {
            (void) pthread_mutex_unlock(&cfv->cv_lock);
            (void) pthread_mutex_unlock(&cfv->write_lock);
            free(wrapper);
            return err;
        }
    }
    if ((err = pthread_mutex_unlock(&cfv->cv_lock)) != 0) {
        (void) pthread_mutex_unlock(&cfv->write_lock);
        free(wrapper);
        return err;
    }

    /* Update that now quiescent slot */
    (void) atomic_cas_ptr(&v->wrapped, old_wrapper, wrapper);

    /* Update current slot pointer */
    (void) atomic_inc_64_nv(&cfv->version);

    /* Done */
    err = pthread_mutex_unlock(&cfv->write_lock);

    /* Release the old cf */
    wrapper_free(old_wrapper);
    return err;
}

