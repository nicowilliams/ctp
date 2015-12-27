
#ifndef TSGV_PRIV_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>
#include "thread_safe_global.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vwrapper {
    pthread_var_destructor_np_t dtor;
    void                        *ptr;       /* the actual value */
    uint32_t                    nref;       /* release when drops to 0 */
    uint64_t                    version;
};

struct var {
    uint32_t            nreaders;   /* no. of readers active in this slot */
    struct vwrapper     *wrapper;   /* wraps real ptr, has nref */
    struct var          *other;     /* always points to the other slot */
    uint64_t            version;
};

struct pthread_var_np {
    pthread_key_t               tkey;           /* to detect thread exits */
    pthread_mutex_t             write_lock;     /* one writer at a time */
    pthread_mutex_t             cv_lock;        /* to signal waiting writer */
    pthread_mutex_t             waiter_lock;    /* to signal waiters */
    pthread_cond_t              cv;             /* to signal waiting writer */
    pthread_cond_t              waiter_cv;      /* to signal waiters */
    struct var                  vars[2];        /* writer only */
    pthread_var_destructor_np_t dtor;           /* both read this */
    uint64_t                    next_version;   /* both read; writer writes */
};

#ifdef __cplusplus
}
#endif

#endif /* TSGV_PRIV_H */
