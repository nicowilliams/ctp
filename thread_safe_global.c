
/*
 * See .h for description of algorithm.
 *
 * To set a new ptr for a pthread_cfvar_np_t variable named foo we:
 *   acquire foo->write_lock;
 *   acquire foo->cv_lock;
 *   atomic increment foo->writer;
 *   find x such that foo->vers[x].vers < foo->vers[(x + 1) % 2].vers;
 *   old_ptr = foo->vers[x].ptr;
 *   foo->vers[x].ptr = new_ptr;
 *   foo->vers[x].vers = foo->vers[(x + 1) % 2].vers + 1;
 *   producer membar;
 *   while foo->vers[(x + 1) % 2].readers > 0
 *     cond_wait(foo->cv, foo->cv_lock);
 *   atomic decrement foo->writer;
 *   release foo->cv_lock;
 *   release foo->write_lock;
 *   foo->put_ref(old_ptr);
 *   return;
 *
 * To get the newest available ptr from foo:
 *   atomic increment foo->vers[0].readers;
 *   atomic increment foo->vers[1].readers;
 *   writer_waiting = atomic_read(foo->writer); (i.e., another membar_consumer)
 *   membar_consumer;
 *   find highest highest foo->vers[x].vers;
 *   atomic decrement foo->[(x + 1) % 2].readers;
 *   find x such that foo->vers[x].vers > foo->vers[(x + 1) % 2].vers;
 *   ptr = foo->vers[x].ptr;
 *   foo->get_ref(ptr);
 *   atomic decrement foo->[x].readers, if now zero then
 *     membar_consumer;
 *     if (foo->vers[x].vers < foo->vers[(x + 1) % 2].vers)
 *       if (writer_waiting || atomic_read(foo->writer))
 *         acquire foo->cv_lock;
 *         cond_signal(foo->cv);
 *         drop foo->cv_lock;
 *   return ptr;
 */

#include "thread_safe_global.h"

/**
 * Initialize a configuration variable
 *
 * @param cfvar Pointer to configuration variable
 * @param get_ref Pointer to function that gets a reference to cf data
 * @param put_ref Pointer to function that releases a reference to cf data, destroying the configuration data on last release
 * @param cfdata Pointer to configuration data
 */
void pthread_cfvar_init_np(pthread_cfvar_np_t *cfvar,
			   pthread_cfvar_ref_np_t get_ref,
			   pthread_cfvar_ref_np_t put_ref,
                           void *cfdata)
{
    memset(cfvar, 0, sizeof (*cfvar));
    /* Init mutexes, cond var */
}

/**
 * Destroy a configuration variable
 */
void pthread_cfvar_destroy_np(pthread_cfvar_np_t *cfvar);

/**
 * Get the most up to date configuration data
 *
 * @param [in] cfvar Pointer to a cf var
 * @param [out] version Pointer to 64-bit integer where the current version will be output
 *
 * @return Pointer to configuration data
 */
void *pthread_cfvar_get_np(pthread_cfvar_np_t *cfvar, uint64_t *version);

/**
 * Release configuration data
 *
 * @param cfvar Pointer to configuration variable
 * @param cfdata Pointer to configuration data
 */
void pthread_cfvar_release_np(pthread_cfvar_np_t *cfvar, void *cfdata);

/**
 * Set new data on a configuration variable
 *
 * @param [in] cfvar Pointer to configuration variable
 * @param [in] version Version number, must be 1 greater than that last obtained with get function
 * @param [in] cfdata Pointer to configuration data
 *
 * @return 0 on success, EEXIST if there's a conflict, or a system error such as ENOMEM.
 */
int pthread_cfvar_set_np(pthread_cfvar_np_t *cfvar, uint64_t version,
			 void *cfdata);

