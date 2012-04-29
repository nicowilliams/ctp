
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
    uint32_t                        nref;
    uint64_t                        version;
    void                            *ptr;
};

struct cfvar {
    uint32_t            nreaders;
    struct cfwrapper    *wrapped_cf; // has nref, wraps real ptr
    struct cfvar        *other;
};

typedef struct pthread_cfvar_np {
    pthread_key_t                   tkey;
    pthread_mutex_t                 write_lock; /* one writer at a time */
    pthread_mutex_t                 cv_lock;    /* to signal waiting writer */
    pthread_cond_t                  cv;         /* to signal waiting writer */
    struct cfvar                    *current;   /* both read; writer writes */
    struct cfvar                    vars[2];    /* writer only */
    pthread_cfvar_destructor_np_t   dtor;       /* both read this */
    uint64_t                        version;    /* writer only */
} pthread_cfvar_np_t;

/**
 * A pthread_cfvar_np_t is an object that holds a pointer to what would
 * typically be configuration information.  Threads can safely read and
 * write this via pthread_cfvar_get_np() and pthread_cfvar_set_np(),
 * with configuration values destroyed only when the last reference is
 * released.
 *
 * Writes are serialized.  Readers don't block, and mostly perform only
 * fast atomic operations; the only blocking operations done by readers
 * are for uncontended resources.
 */
typedef struct pthread_cfvar_np pthread_cfvar_np_t;

int pthread_cfvar_init_np(pthread_cfvar_np_t *, pthread_cfvar_destructor_np_t);
void pthread_cfvar_destroy_np(pthread_cfvar_np_t *);
int pthread_cfvar_get_np(pthread_cfvar_np_t *, void **, uint64_t *);
void pthread_cfvar_release_np(pthread_cfvar_np_t *);
int pthread_cfvar_set_np(pthread_cfvar_np_t *, void *, uint64_t *);

#ifdef __cplusplus
}
#endif

#endif /* URCU_H */
