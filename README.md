What is it?  A "thread-safe global variable" for C
--------------------------------------------------

A thread-safe global variable lets readers keep using a value read from
it until they read the next value.  Values are automatically destroyed
when the last reference is released or the last referring thread exits.
This is not unlike a Clojure "ref".  Reads are always fast and don't
block writes; conversely writes are serialized but otherwise fast and
never block reads.

    typedef struct pthread_var_np *pthread_var_np_t;
    typedef void (*pthread_var_destructor_np_t)(void *);
    int  pthread_var_init_np(pthread_var_np_t *var, pthread_var_destructor_np_t value_destructor);
    void pthread_var_destroy_np(pthread_var_np_t var);
    int  pthread_var_get_np(pthread_var_np_t var, void **valuep, uint64_t *versionp);
    int  pthread_var_set_np(pthread_var_np_t var, void *value, uint64_t *versionp);
    int  pthread_var_wait_np(pthread_var_np_t var);
    void pthread_var_release_np(pthread_var_np_t var);

Why?  Because read-write locks are teh worst
--------------------------------------------

So you have rarely-changing global data (e.g., loaded configuration,
plugins, ...), and you have many threads that read this, and you want
reads to be fast.  Worker threads need stable configuration/whatever
while doing work, then when they pick up another task they can get a
newer configuration if there is one.  How would you implement that?  A
safe answer is: read-write locks around reading/writing the global
variable, and reference count the data.  But read-write locks are
inherently bad: readers either can starve writers or can be blocked by
writers.

A thread-safe global variable, on the other hand, is always fast to
read, even when there's an active writer, and reading does not starve
writers.

How?
----

Two implementations are included at this time.  Read the source to find
out more.

The two implementations have slightly different characteristics.  One
has O(1) reads and writes, but readers call free() and, under certain
circumstances, sometimes have to signal a potentially-waiting writer --
a blocking operation, though on uncontended resources.  The other has
O(1) reads, and O(N) writes (where N is the number of live threads that
have read the variable), with readers never calling the allocator after
the first read in any given thread, and writers never calling the
allocator while holding a lock.

Both implementations perform equally well on the included test.  The
test pits 20 reader threads waiting various small amounts of time
between reads (one not waiting at all), against 4 writer threads waiting
various small amounts of time between writes.  This test found a variety
of bugs during development.  In both cases writes are, on average, 5x
slower than reads, and reads are in the ten microseconds range on an old
laptop, running under virtualization.

A test program is included that hammers the implementation.  Run it in a
loop, with and without valgrind.

Install
-------

Right now there's no Makefile, sorry, but it's trivial to build it:

    $ cc -Wall -Wextra -ggdb3 -DHAVE___ATOMIC -c *.c && cc -o t *.o -lpthread

or

    $ cc -Wall -Wextra -ggdb3 -DUSE_URCU_SUBSCRIPTION_SLOTS_DESIGN -DHAVE___ATOMIC -c *.c && cc -o t *.o -lpthread

An build configuration system is needed, in part to select an atomic primitive backend.

Several atomic primitives implementations are available:

 - gcc/clang `__atomic`
 - gcc/clang `__sync`
 - Win32 `Interlocked*()`
 - global pthread mutex
 - no synchronization (watch the test blow up!)

TODO
----

 - Add an implementation using read-write locks to compare performance with
 - Use symbol names that don't conflict with pthread
 - Rename atomic utilities to not conflict with known atomics libraries