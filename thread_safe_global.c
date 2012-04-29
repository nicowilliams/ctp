
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include "thread_safe_global.h"

static void
wrapper_free(struct cfwrapper *wrapper)
{
    if (wrapper == NULL)
        return;
    if (wrapper != NULL && atomic_dec_uint32(&wrapper->nref) == 0) {
        if (wrapper->dtor != NULL)
            wrapper->dtor(wrapper);
        free(wrapper);
    }
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

    if ((err = pthread_key_create(&cfv->tkey, cfvar_dtor_wrapper)) != 0)
        return err;
    if ((err = pthread_mutex_init(&cfv->write_lock, NULL)) != 0)
        return err;
    if ((err = pthread_mutex_init(&cfv->cv_lock, NULL)) != 0) {
        pthread_mutex_destroy(&cfv->write_lock);
        return err;
    }
    if ((err = pthread_cond_init(&cfv->cv, NULL)) != 0) {
        pthread_mutex_destroy(&cfv->write_lock);
        pthread_mutex_destroy(&cfv->cv_lock);
        return err;
    }
    cfv->vars[0].nreaders = 0;
    cfv->vars[0].wrapped_cf = NULL;
    cfv->vars[0].other = &cfv->vars[1];
    cfv->vars[1].nreaders = 0;
    cfv->vars[1].wrapped_cf = NULL;
    cfv->vars[1].other = &cfv->vars[0];
    cfv->current = NULL;
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
    /* CFs will be release by the thread-specifics' destructors */
    /* We leak cfvar->tkey */
}

/**
 * Get the most up to date configuration data
 *
 * @param [in] cfvar Pointer to a cf var
 * @param [out] res Pointer to location where configuration data will be output
 * @param [out] version Pointer to 64-bit integer where the current version will be output
 *
 * @return Pointer to configuration data
 */
int pthread_cfvar_init_np(pthread_cfvar_np_t *, pthread_cfvar_destructor_np_t);
void pthread_cfvar_destroy_np(pthread_cfvar_np_t *);
int pthread_cfvar_get_np(pthread_cfvar_np_t *, void **, uint64_t *);
int
pthread_cfvar_get_np(pthread_cfvar_np_t *cfv, void **res, uint64_t *version)
{
    int err;
    size_t i;
    struct cfvar *v;

    *res = NULL;
    *version = 0;
    v = cfv->current;
    if (v == NULL)
        return 0;
    (void) atomic_inc_uint32(&v->nreaders);
    for (i = 2; i > 0; i--) {
        if (v->wrapped_cf == NULL) {
            /* Whoa, nothing there; shouldn't happen */
            (void) atomic_dec_uint32(&v->nreaders);
            return 0;
        }
        if (atomic_inc_uint32(&v->wrapped_cf->nref) > 1) {
            /* Done */
            if ((err = pthread_setspecific(cfv->tkey, v->wrapped_cf)) != 0)
                return err;
            *version = v->wrapped_cf->version;
            *res = v->wrapped_cf->ptr;
            return 0;
        }
        /*
         * We're racing with a writer.
         *
         * We increment the reader count on the other v before
         * decrementing the reader count on this one.  This should
         * guarantee that we find the other one present if this one was
         * being replaced.
         */
        atomic_inc_uint32(&v->other->nreaders);
        if (atomic_dec_uint32(&v->nreaders) == 0) {
            /*
             * As the last reader of this v we have to signal the
             * writer that may be waiting for us to stop reading.
             */
            if ((err = pthread_mutex_lock(&cfv->cv_lock)) != 0) {
                atomic_dec_uint32(&v->other->nreaders);
                return err;
            }

            wrapper_free(v->wrapped_cf);
            v->wrapped_cf = NULL;

            if ((err = pthread_cond_broadcast(&cfv->cv)) != 0) {
                atomic_dec_uint32(&v->other->nreaders);
                (void) pthread_mutex_unlock(&cfv->cv_lock);
                return err;
            }
            if ((err = pthread_mutex_unlock(&cfv->cv_lock)) != 1) {
                atomic_dec_uint32(&v->other->nreaders);
                return err;
            }
        }
        v = v->other;
    }
    return EAGAIN; /* shouldn't happen! */
}

/**
 * Release this thread's reference to the current value of the given
 * configuration variable.
 *
 * @param cfv [in] A configuration variable
 */
void
pthread_cfvar_release_np(pthread_cfvar_np_t *cfv)
{
    wrapper_free(pthread_getspecific(cfv->tkey));
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
     * Starting the new value with nref == 1 is critical.
     *
     * Readers take the fast path when they incred this and get
     * something > 1.  Writers release this one reference when they
     * replace the value in the old slot, and race with the last reader
     * to actually free it (the race is serialized, naturally; the one
     * who gets nref == 0 destroys the value).
     */
    wrapper->nref = 1;
    wrapper->dtor = cfv->dtor;
    wrapper->ptr = cfdata;

    if ((err = pthread_mutex_lock(&cfv->write_lock)) != 0) {
        free(wrapper);
        return err;
    }

    wrapper->version = ++(cfv->version);

    if (cfv->current == NULL) {
        /* This is the first cf being written; easy */
        v = &cfv->vars[0];
        v->wrapped_cf = wrapper;
        cfv->current = v;
        return pthread_mutex_unlock(&cfv->write_lock);
    }

    /* Grab the other slot */
    v = cfv->current->other;
    old_wrapper = cfv->current->wrapped_cf;

    /* Wait until that slot is quiescent before mutating it */
    if ((err = pthread_mutex_lock(&cfv->cv_lock)) != 0) {
        (void) pthread_mutex_unlock(&cfv->write_lock);
        free(wrapper);
        return err;
    }
    while (v->nreaders > 0) {
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
    v->wrapped_cf = wrapper;
    cfv->current = v;       /* new cf now visible */

    /* Done */
    err = pthread_mutex_unlock(&cfv->write_lock);

    /* Release the old cf */
    wrapper_free(old_wrapper);
    return err;
}

