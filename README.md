Q: What is it?  A: A "thread-safe global variable" for C
--------------------------------------------------------

A thread-safe global variable lets readers keep using a value read from
it until they read the next value.  Memory management is automatic:
values are automatically destroyed when the last reference is released
or the last referring thread exits.  Reads are always fast and never
block writes.  Writes are serialized but otherwise also always fast and
never block reads.

This is not unlike a Clojure "ref".  It's also similar to RCU, but
unlike RCU, this has a much simpler API with nothing like
`synchronize_rcu()`, and doesn't require any cross-CPU calls nor the
ability to make CPUs/threads run, and it has no application-visible
concept of critical sections, therefore it works in user-land with no
special kernel support.

 - One thread needs to create the variable by calling
   `pthread_var_init_np()` and providing a value destructor.
 - Most threads only ever need to call `pthread_var_get_np()`, and maybe
   once `pthread_var_wait_np()` to wait until at least one value has
   been set.
 - One or more threads may call `pthread_var_set_np()` to publish new
   values.

The API is:

    typedef struct pthread_var_np *pthread_var_np_t;
    typedef void (*pthread_var_destructor_np_t)(void *);
    int  pthread_var_init_np(pthread_var_np_t *var, pthread_var_destructor_np_t value_destructor);
    void pthread_var_destroy_np(pthread_var_np_t var);
    int  pthread_var_get_np(pthread_var_np_t var, void **valuep, uint64_t *versionp);
    int  pthread_var_set_np(pthread_var_np_t var, void *value, uint64_t *versionp);
    int  pthread_var_wait_np(pthread_var_np_t var);
    void pthread_var_release_np(pthread_var_np_t var);

Value version numbers increase monotonically when set.

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

The two implementations have slightly different characteristics.

 - One implementation ("slot pair") has O(1) reads and writes.

   But readers call free() and the value destructor, and, sometimes have
   to signal a potentially-waiting writer -- a blocking operation,
   though on uncontended resources.

   This implementation has a pair of slots, one containing the "current"
   value and one containing the "previous"/"next" value.  Writers make the
   "previous" slot into the next "current" slot, and readers read from
   whichever slot appears to be the current slot.  Values are wrapped
   with a wrapper that includes a reference count, and they are released
   when the reference count drops to zero.

   The trick is that writers will wait until the number of active
   readers of the previous slot is zero.  Thus the last reader of a
   previous slot must signal a potentially-awaiting writer (which
   requires taking a lock that the awaiting writer should have
   relinquished in order to wait).  Thus reading is mostly lock-less and
   never blocks on contended resources.

 - The other implementation ("slot list") has O(1) reads, and O(N)
   writes (where N is the maximum number of live threads that have read
   the variable), with readers never calling the allocator after the
   first read in any given thread, and writers never calling the
   allocator while holding a lock.

   But readers have to loop over their fast path, a loop that can run
   indefinitely if there are higher-priority writers who starve the
   reader of CPU time.

   This implementation has a list of referenced values, with the head of
   the list always being the current one, and a list of "subscription"
   slots, one per-reader thread.  Readers allocate a slot on first read,
   and thence copy the head of the values list to their slots.  Writers
   have to perform garbage collection on the list of referenced values.

   Subscription slot allocation is lock-less.  Indeed, everything is
   lock-less in the reader.

A test program is included that hammers the implementation.  Run it in a
loop, with or without valgrind, ASAN (address sanitizer), or other
memory checkers, to look for racy bugs.

Both implementations perform similarly well on the included test.

The test pits 20 reader threads waiting various small amounts of time
between reads (one not waiting at all), against 4 writer threads waiting
various small amounts of time between writes.  This test found a variety
of bugs during development.  In both cases writes are, on average, 5x
slower than reads, and reads are in the ten microseconds range on an old
laptop, running under virtualization.

Performance
-----------

On an old i7 laptop, virtualized, reads on idle thread-safe global
variables (i.e., no writers in sight) take about 15ns.  This is because
the fast path in both implementations consists of reading a thread-local
variable and then performing a single acquire-fenced memory read.

On that same system, when threads write very frequently then reads slow
down to about 8us (8000ns).  (But the test had eight times more threads
than CPUs, so the cost of context switching is included in that number.)

On that same system writes on a busy thread-safe global variable take
about 50us (50000ns), but non-contending writes on an otherwise idle
thread-safe global variable take about 180ns.

I.e., this is blindingly fast, especially for intended use case
(infrequent writes).

Install
-------

Clone this repo, select a configuration, and make it.

For example, to build the slot-pair implementation, use:

    $ make clean slotpair

To build the slot-list implementation, use:

    $ make clean slotlist

A GNU-like make(1) is needed.

Configuration variables:

 - `COPTFLAG`
 - `CDBGFLAG`
 -`CC`
 - `ATOMICS_BACKEND`

   Values: `-DHAVE___ATOMIC`, `-DHAVE___SYNC`, `-DHAVE_INTEL_INTRINSICS`, `-DHAVE_PTHREAD`, `-DNO_THREADS`

 - `TSGV_IMPLEMENTATION`

   Values: `-DUSE_TSGV_SLOT_PAIR_DESIGN`, `-DUSE_TSGV_SUBSCRIPTION_SLOTS_DESIGN`

A build configuration system is needed, in part to select an atomic
primitive backend.

Several atomic primitives implementations are available:

 - gcc/clang `__atomic`
 - gcc/clang `__sync`
 - Win32 `Interlocked*()`
 - Intel compiler intrinsics (`_Interlocked*()`)
 - global pthread mutex
 - no synchronization (watch the test blow up!)

TODO
----

 - Add a proper build system
 - Add an implementation using read-write locks to compare performance
   with
 - Use symbol names that don't conflict with pthread
 - Use symbol names that don't conflict with known atomics libraries (so
   those can be used as an atomics backend)
 - Support Win32 (perhaps by building a small pthread compatibility
   library; only mutexes and condition variables are needed)

