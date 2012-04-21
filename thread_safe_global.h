
/*
 * This is an attempt at a MT-hot "current configuration" API with
 * thread-safe updates.  Readers do nothing that blocks except for the
 * last reader when there's a waiting writer, in which case the reader
 * grabs a lock, signals the writer, then drops the lock and goes on its
 * merry way.  This isn't just for configuration though: it could be
 * used for lots of things, such as the global table of string constants
 * in libheimbase (see the HSTR() macro used in libheimbase).
 *
 * This could also be used for a COW approach to global state.  Suppose
 * you want to update something deep in global state, suppose in fact
 * that you have a tree of data using something like libheimbase objects
 * (or Core Foundation objects), and you want to update a leaf object
 * using an XPath-like API.  But this being global state means you need
 * synchronization.  If the XPath-like API includes a COW version that
 * returns a new root then you can just use this API here to replace the
 * global root.
 *
 * This is a better interface than reader/writer locks.  There can be no
 * writer starvation.  Readers mostly do not block.  Only the last
 * reader of a given version of configuration data can block, and only
 * at times when there are two or more writes in quick succession.
 * And such a reader it will wait only for the one active writer, if
 * there is one.
 *
 * If multiple readers race with a writer some may get the configuration
 * data that was in effect before the writer updates it, and some may
 * get the configuration data that the writer sets.  But once a writer
 * is [almost] finished then all readers will get the new configuration
 * data until the next writer comes along.
 *
 * See Doxygen comments below for API details.
 *
 * typedef struct pthread_cfvar_np pthread_cfvar_np_t;
 * void pthread_cfvar_init_np(pthread_cfvar_np_t *cfvar,
 *                            void (*get_ref)(void *), # ref count increment
 *                            void (*put_ref)(void *), # ref count decrement
 *                            void *cfdata             # global variable
 *                            );
 * void *pthread_cfvar_get_np(pthread_cfvar_np_t *cfvar, uint64_t *version);
 * void *pthread_cfvar_release_np(pthread_cfvar_np_t *cfvar, void *cfdata);
 * int pthread_cfvar_set_np(pthread_cfvar_np_t *cfvar, uint64_t *version,
 *                          void *cfdata);
 *
 * "cf" here means "configuration", since the purpose of this API is to
 * get the most current configuration for an application or library.
 *
 * A cfvar holds configuration data.  A cfdata is a pointer to data of
 * an application-specific type.
 *
 * The 'get' function is guaranteed not to contend for any resources.
 * A single get call may block, but only as it races with a 'set' call.
 *
 * The 'set' call is guaranteed to be serialized relative to other 'set'
 * calls.
 *
 * Callers of the 'set' function should first 'get' the current
 * configuration data to get the current version, then increment the
 * version, then call the 'set' function.  The version number may wrap
 * (not likely though, being 64-bit...), but will never be output as
 * anything larger than UINT64_MAX - 2, nor as zero, except when no
 * configuration has been set, in which case the version will be
 * reported as zero.
 *
 * There is no guarantee that by the time the 'set' function returns all
 * other threads are using the new configuration data.  The caller may
 * arrange to obtain such guarantees by acquiring a mutex before calling
 * the 'set' funciton then blocking on a condition variable that will be
 * signaled by the last reference release of the previous configuration.
 * The caller may need to arrange to wake up all threads (e.g., that are
 * waiting in an event loop) in order to make such a synchronization
 * mechanism timely.
 *
 * The way this works is that the cfvar holds two versions of
 * configuration data.  Readers will always get the newest version.
 * Writers replace what used to be the oldest version.  See the struct
 * definition below this comment block, then the algorithm immediately
 * below:
 *
 * To set a new ptr for a pthread_cfvar_np_t variable named foo we:
 *   acquire foo->write_lock; (exclude other writers)
 *   acquire foo->cv_lock; (used for waiting for last reader of old config)
 *   atomic increment foo->writer;
 *
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
 *   membar_consumer;
 *   find x such that foo->vers[x].vers > foo->vers[(x + 1) % 2].vers;
 *   atomic decrement foo->[(x + 1) % 2].readers;
 *   ptr = foo->vers[x].ptr;
 *   foo->get_ref(ptr);
 *   if (atomic decrement foo->[x].readers now zero &&
 *       atomic_read(foo->writer)
 *     membar_consumer;
 *     acquire foo->cv_lock; (if racing with a writer this is where we
 *                            serialize w.r.t. them)
 *     cond_signal(foo->cv); (if there's no writer waiting, no sweat)
 *     drop foo->cv_lock;
 *   return ptr;
 *
 * Note that no writer starvation is possible because the last reader of
 * an old configuration data version will notice that it is the last
 * reader, and will signal the waiting writer, and new readers will be
 * getting the new version if they start after the writer updates the
 * new version, thus new readers can't block the waiting writer.
 * Writers only ever block on the last reader of the old configuration.
 * Why would there be a reader getting the old config?  Well, 
 *
 * Note that readers can notice a version change that happened after
 * their selection of a version; readers could re-start to get the new
 * version, but this might cause problems (e.g., readers never finishing
 * because of a constant stream of writers).
 *
 * What's missing?  Ah, we need to pthread_cancel_push/pop() around the
 * bodies of the get and set functions so that we can avoid leaving
 * cfvars in bad state, or leak references to cf data when the thread
 * gets cancelled.  The caller is responsible for doing the same to
 * avoid leaking references to cf data, but most apps don't bother with
 * this -- we have to because we aim to be a library utility.
 */

typedef void (*pthread_cfvar_ref_np_t)(void *);

struct cfvar_vers {
    AO_t	vers;
    AO_t	readers;
    void	*ptr;
};

typedef struct pthread_cfvar_np {
    pthread_mutex_t		write_lock;
    pthread_mutex_t		cv_lock;
    pthread_cond_t		cv;
    pthread_cfvar_ref_np_t	get_ref;
    pthread_cfvar_ref_np_t	put_ref;
    AO_t			writer;
    struct cfvar_vers		vers[2];
} pthread_cfvar_np_t;

typedef struct pthread_cfvar_np pthread_cfvar_np_t;

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
                           void *cfdata);

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
 * Release configuration data (like setting NULL as the new value)
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

