
#ifndef URCU_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* _np == non-portable, really, non-standard extension */
typedef void (*pthread_var_destructor_np_t)(void *);

/* XXX Make private */
struct vwrapper {
    pthread_var_destructor_np_t dtor;
    void                        *ptr;       /* the actual value */
    uint32_t                    nref;       /* release when drops to 0 */
    uint64_t                    version;
};

/*
 * XXX Can't make private without making struct pthread_var_np below
 * private too.  Then the typedef would have to be pointer type.
 *
 * But we can't pollute the namespace.
 */
struct var {
    uint32_t            nreaders;   /* no. of readers active in this slot */
    struct vwrapper     *wrapper;   /* wraps real ptr, has nref */
    struct var          *other;     /* always points to the other slot */
    uint64_t            version;
};

/*
 * Copying the pthread style means that some of the internals are
 * exposed in the ABI.  Maybe we shouldn't do that.
 */
typedef struct pthread_var_np {
    pthread_key_t               tkey;           /* to detect thread exits */
    pthread_mutex_t             write_lock;     /* one writer at a time */
    pthread_mutex_t             cv_lock;        /* to signal waiting writer */
    pthread_mutex_t             waiter_lock;    /* to signal waiters */
    pthread_cond_t              cv;             /* to signal waiting writer */
    pthread_cond_t              waiter_cv;      /* to signal waiters */
    struct var                  vars[2];        /* writer only */
    pthread_var_destructor_np_t dtor;           /* both read this */
    uint64_t                    next_version;   /* both read; writer writes */
} pthread_var_np_t;

/**
 * A pthread_var_np_t is an object that holds a pointer to what would
 * typically be configuration information.  Thus a pthread_var_np_t is
 * a thread-safe "global variable".  Threads can safely read and write
 * this via pthread_var_get_np() and pthread_var_set_np(), with
 * configuration values destroyed only when the last reference is
 * released.  Applications should store mostly read-only values in a
 * pthread_var_np_t -- typically configuration information, the sort of
 * data that rarely changes.
 *
 * Writes are serialized.  Readers don't block and do not spin, and
 * mostly perform only fast atomic operations; the only blocking
 * operations done by readers are for uncontended resources.
 */
typedef struct pthread_var_np pthread_var_np_t;

int  pthread_var_init_np(pthread_var_np_t *, pthread_var_destructor_np_t);
void pthread_var_destroy_np(pthread_var_np_t *);

int  pthread_var_get_np(pthread_var_np_t *, void **, uint64_t *);
int  pthread_var_wait_np(pthread_var_np_t *);
int  pthread_var_set_np(pthread_var_np_t *, void *, uint64_t *);
void pthread_var_release_np(pthread_var_np_t *);

#ifdef __cplusplus
}
#endif

#endif /* URCU_H */
