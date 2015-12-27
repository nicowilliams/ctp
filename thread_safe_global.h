
#ifndef URCU_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* _np == non-portable, really, non-standard extension */


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
typedef struct pthread_var_np *pthread_var_np_t;

typedef void (*pthread_var_destructor_np_t)(void *);

int  pthread_var_init_np(pthread_var_np_t *, pthread_var_destructor_np_t);
void pthread_var_destroy_np(pthread_var_np_t);

int  pthread_var_get_np(pthread_var_np_t, void **, uint64_t *);
int  pthread_var_wait_np(pthread_var_np_t);
int  pthread_var_set_np(pthread_var_np_t, void *, uint64_t *);
void pthread_var_release_np(pthread_var_np_t);

#ifdef __cplusplus
}
#endif

#endif /* URCU_H */
