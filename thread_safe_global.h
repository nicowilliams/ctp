
#ifndef URCU_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* _np == non-portable, really, non-standard extension */
typedef void (*pthread_cfvar_destructor_np_t)(void *);

/* XXX Make private? */
struct cfwrapper {
    pthread_cfvar_destructor_np_t   dtor;
    void                            (*wrapper_dtor)(void *);
    void                            *ptr;       /* the actual value */
    uint32_t                        nref;       /* release when drops to 0 */
    uint64_t                        version;
};

struct cfvar {
    uint32_t            nreaders;   /* no. of active readers */
    struct cfwrapper    *wrapped;   /* has nref, wraps real ptr */
    struct cfvar        *other;     /* points to the other slot */
};

/*
 * Copying the pthread style means that some of the internals are
 * exposed in the ABI.  Maybe we shouldn't do that.
 */
typedef struct pthread_cfvar_np {
    pthread_key_t                   tkey;
    pthread_mutex_t                 write_lock; /* one writer at a time */
    pthread_mutex_t                 cv_lock;    /* to signal waiting writer */
    pthread_mutex_t                 waiter_lock;/* to signal waiters */
    pthread_cond_t                  cv;         /* to signal waiting writer */
    pthread_cond_t                  waiter_cv;  /* to signal waiters */
    struct cfvar                    vars[2];    /* writer only */
    pthread_cfvar_destructor_np_t   dtor;       /* both read this */
    uint64_t                        version;    /* both read; writer writes */
    uint32_t                        waiters;    /* unused */
} pthread_cfvar_np_t;

/**
 * A pthread_cfvar_np_t is an object that holds a pointer to what would
 * typically be configuration information.  Thus a pthread_cfvar_np_t is
 * a thread-safe "configuration variable".  Threads can safely read and
 * write this via pthread_cfvar_get_np() and pthread_cfvar_set_np(),
 * with configuration values destroyed only when the last reference is
 * released.  Applications should store mostly read-only values in a
 * pthread_cfvar_np_t -- typically configuration information, the sort
 * of data that rarely changes.
 *
 * Writes are serialized.  Readers don't block and do not spin, and
 * mostly perform only fast atomic operations; the only blocking
 * operations done by readers are for uncontended resources.
 */
typedef struct pthread_cfvar_np pthread_cfvar_np_t;

int  pthread_cfvar_init_np(pthread_cfvar_np_t *, pthread_cfvar_destructor_np_t);
void pthread_cfvar_destroy_np(pthread_cfvar_np_t *);

int  pthread_cfvar_get_np(pthread_cfvar_np_t *, void **, uint64_t *);
int  pthread_cfvar_wait_np(pthread_cfvar_np_t *);
int  pthread_cfvar_set_np(pthread_cfvar_np_t *, void *, uint64_t *);
void pthread_cfvar_release_np(pthread_cfvar_np_t *);

#ifdef __cplusplus
}
#endif

#endif /* URCU_H */
