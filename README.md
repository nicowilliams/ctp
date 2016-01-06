Q: What is it?  A: A "thread-safe global variable" (TSGV) for C
---------------------------------------------------------------

This repository's main feature is a thread-safe global variable (TSGV)
for C.  More C thread-primitives may be added in the future, thus the
repository's name.

A TSGV lets readers keep using a value read from it until they read the
next value.  Memory management is automatic: values are automatically
destroyed when the last reference is released (explicitly, or implicitly
at the next read, or when a reader thread exits).  Reads are *lock-less*
and fast, and _never block writes_.  Writes are serialized but otherwise
interact with readers without locks, thus writes *do not block reads*.

This is not unlike a Clojure "ref".  It's also similar to RCU, but
unlike RCU, this has a much simpler API with nothing like
`synchronize_rcu()`, and doesn't require any cross-CPU calls nor the
ability to make CPUs/threads run, and it has no application-visible
concept of critical sections, therefore it works in user-land with no
special kernel support.

 - One thread needs to create the variable by calling
   `pthread_var_init_np()` and providing a value destructor.  There is
   no static initializer, though one could be added.
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

Value version numbers increase monotonically when values are set.

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

Two implementations are included at this time.

The two implementations have slightly different characteristics.

 - One implementation ("slot pair") has O(1) lock-less and spin-less
   reads and O(1) serialized writes.

   But readers call free() and the value destructor, and, sometimes have
   to signal a potentially-waiting writer -- a blocking operation,
   though on an uncontended resource (so not really blocking, but it
   does involve a system call).

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

   Values are reference counted and so released immediately when the
   last reference is dropped.

 - The other implementation ("slot list") has O(1) lock-less (but
   spinning) reads, and O(N log(M)) serialized writes where N is the maximum
   number of live threads that have read the variable and M is the
   number of referenced values).
   
   Readers never call the allocator after the first read in any given
   thread, and writers never call the allocator while holding the writer
   lock.

   Readers have to loop over their fast path, a loop that can run
   indefinitely if there are infinitely many higher-priority writers who
   starve the reader of CPU time.  To help avoid this, writers yield the
   CPU before relinquishing the write lock.

   This implementation has a list of referenced values, with the head of
   the list always being the current one, and a list of "subscription"
   slots, one per-reader thread.  Readers allocate a slot on first read,
   and thence copy the head of the values list to their slots.  Writers
   have to perform garbage collection on the list of referenced values.

   Subscription slot allocation is lock-less.  Indeed, everything is
   lock-less in the reader.

   Values are released at the first write after the last reference is
   dropped, as values are garbage collected by writers.

The first implementation written was the slot-pair implementation.  The
slot-list design is much easier to understand on the read-side, but it
is significantly more complex on the write-side.

Requirements
------------

C89, POSIX threads (though TSGV should be portable to Windows),
compilers with atomics intrinsics and/or atomics libraries.

In the future this may be upgraded to a C99 or even C11 requirement.

Testing
-------

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

    $ make CPPDEFS=-DHAVE_SCHED_YIELD clean slotlist

A GNU-like make(1) is needed.

Configuration variables:

 - `COPTFLAG`
 - `CDBGFLAG`
 -`CC`
 - `ATOMICS_BACKEND`

   Values: `-DHAVE___ATOMIC`, `-DHAVE___SYNC`, `-DHAVE_INTEL_INTRINSICS`, `-DHAVE_PTHREAD`, `-DNO_THREADS`

 - `TSGV_IMPLEMENTATION`

   Values: `-DUSE_TSGV_SLOT_PAIR_DESIGN`, `-DUSE_TSGV_SUBSCRIPTION_SLOTS_DESIGN`

 - `CPPDEFS`

   Other options, mainly: what yield() implementation to use
   (`-DHAVE_PTHREAD_YIELD`, `-DHAVE_SCHED_YIELD`, or `-DHAVE_YIELD`).
   This is needed for the slot-list implementation.

   `CPPDEFS` can also be used to set `NDEBUG`.

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

 - Add an attributes optional input argument to the init function.

   Callers should be able to express the following preferences:

    - OK for readers to spin, yes or no.        (No  -> slot-pair)
    - OK for readers to alloc/free, yes or no.  (No  -> slot-list, GC)
    - Whether version waiting is desired.       (Yes -> slot-pair)

   On conflict give priority to functionality.

 - Add a version predicate to set or a variant that takes a version
   predicate.  (A version predicate -> do not set the new value unless
   the current value's version number is the given one.)

 - Add an API for waiting for values older than some version number to
   be released
  
   This is tricky for the slot-pair case because we don't have a list of
   extant values, but we need it in order to determine what is the
   oldest live version at any time.  Such a list would have to be
   doubly-linked and updating the double links to remove items would be
   rather difficult to do lock-less-ly and thread-safely.  We could
   defer freeing of list elements so that only tail elements can be
   removed.  When a wrapper's refcount falls to zero, signal any waiters
   who can then garbage collect the lists with the writer lock held and
   find the oldest live version.

   For the slot-list case the tricky part is that unreferenced values
   are only detected when there's a write.  We could add a refcount to
   the slot-list case, so that when refcounts fall to zero we signal any
   waiter(s), but because of the way readers find a current value...
   reference counts could go down to zero then back up, so we must still
   rely on GC to actually free, and we can only rely on refcounts to
   signal a waiter.

   It seems we need a list and refcounts, so that the slot-pair and
   slot-list cases become quite similar, and the only difference
   ultimately is that slot-list can spin while slot-pair cannot.  Thus
   we might want to merge the two implementations, with attributes of
   the variable (see above) determining which codepaths get taken.

   Note too that both implementations can (or do) defer calling of the
   value destructor so that reading is fast.  This should be an option.

 - Make this cache-friendly.  In particular, the slot-list case could
   have the growable slot-array be an array of pointers to slots instead
   of an array of slots.  Or slots could be sized more carefully, adding
   padding if need be.

 - Maybe add a static initializer... as a function-style macro that
   takes a destructor function argument.  This basically means adding a
   `pthread_once_t` to the variable.  C99 would then be required though
   (for the initializer).

 - Add a proper build system.
 - Add an implementation using read-write locks to compare performance
   with.
 - Parametrize the test program.
 - Use symbol names that don't use the `pthread_` prefix, or provide a
   configuration feature for renaming them with C pre-processor macros.
 - Use symbol names that don't conflict with known atomics libraries (so
   those can be used as an atomics backend).  Currently the atomics
   symbols are loosely based on Illumos atomics primitives.
 - Support Win32 (perhaps by building a small pthread compatibility
   library; only mutexes and condition variables are needed).

