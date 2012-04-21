
/*
 * See .h for description of algorithm.
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
 * @param [in] version Version number, must be 0 or current version + 1
 * @param [in] cfdata Pointer to configuration data
 * @param [out] new_version New version number
 *
 * @return 0 on success, EEXIST if there's a conflict, or a system error such as ENOMEM.
 */
int pthread_cfvar_set_np(pthread_cfvar_np_t *cfvar, uint64_t version,
			 void *cfdata, uint64_t *new_version);

