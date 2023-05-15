
> NOTE: This repo is mirrored at https://github.com/cryptonector/ctp and https://github.com/nicowilliams/ctp

# Q: What is it?  A: Lock-free Data Structures and APIs for C, Particularly RCU, Permissively Licensed

This repository's current features are:

 - a lock-free/wait-free read-copy-update (RCU) like, thread-safe
   variable (TSV) for C with a very simple API (get & set)
 - a hazard-pointer data structure and API for C
 - a lock-free array list for C
 - a lock-free "descriptor table" for C (much like POSIX file descriptor
   tables, but for other things than open files)
 - a pthread key multiplexer to avoid running out of pthread keys

Upcoming:

 - a lock-free hash table with similar semantics to the thread-safe
   variable (TSV)

More C thread-primitives may be added in the future, thus the
repository's name being quite generic.

A TSV lets readers safely keep using a value read from the TSV until
they read the next value.  Memory management is automatic: values are
automatically destroyed when the last reference to a value is released
whether explicitly, or implicitly at the next read, or when a reader
thread exits.  References can also be relinquished manually.  Reads are
_lock-less_ and fast, and _never block writers_.  Writers are serialized
but otherwise interact with readers without locks, thus writes *do not
block reads*.

> In one of the two implementations included readers only execute atomic
> memory loads and stores, though they loop over that when racing with a
> writer.  As aligned loads and stores are typically atomic on modern
> archictures, this means no expensive atomic operations are needed --
> not even a single atomic increment or decrement.

This is not unlike a Clojure `ref`, or like a Haskell `msync`.  It's
also similar to RCU, but unlike RCU, this has a very simple API with
nothing like `synchronize_rcu()`, and doesn't require any cross-CPU
calls nor the ability to make CPUs/threads run, and it has no
application-visible concept of critical sections, therefore it works in
user-land with no special kernel support.

> I wrote TSVs back in 2012, and at the time I was not aware of the term
> "hazard pointer" for the technique I used in one of the TSV
> implementations.  Hazard pointers compose and thus scale very well as
> a development technique, so I'm making a first-class hazard pointer
> data structure and API (public API) to help construct other lock-free
> data structures than just TSVs.
>
> Hazard pointers allow one to use a trivial loop over atomically
> reading a shared location and writing to a thread-local "hazard
> pointer" until the latter is seen to have the same value as the
> former.  Values referenced by hazard pointers are not to be destroyed,
> with no need for reference counting.
>
> The main down side to the hazard pointer technique is that a garbage
> collection step is needed occasionally.  For rarely-written pointer
> variables the garbage collection step is only needed when writing.
> For pointer variables written very often the readers will also often
> have to execute the garbage collection.  Garbage collection is O(N)
> where N is the number of live threads that have read a pointer
> variable protected by hazard pointers, so this is not so bad.

 - One thread needs to create the variable (as many as desired) once by
   calling `thread_safe_var_init()` and providing a value destructor.

   > There is currently no static initializer, though one could be
   > added.  One would typically do this early in `main()` or in a
   > `pthread_once()` initializer.

 - Most threads only ever need to call `thread_safe_var_get()`.

   > Reader threads _may_ also call `thread_safe_var_release()` to allow
   > a value to be freed sooner than otherwise.

 - One or more threads may call `thread_safe_var_set()` to set new
   values on the TSVs.

The API is:

```C
    typedef struct thread_safe_var *thread_safe_var;    /* TSV */
    typedef void (*thread_safe_var_dtor_f)(void *);     /* Value destructor */

    /* Initialize a TSV with a given value destructor */
    int  thread_safe_var_init(thread_safe_var *, thread_safe_var_dtor_f);

    /* Get the current value of the TSV and a version number for it */
    int  thread_safe_var_get(thread_safe_var, void **, uint64_t *);

    /* Set a new value on the TSV (outputs the new version) */
    int  thread_safe_var_set(thread_safe_var, void *, uint64_t *);

    /* Optional functions follow */

    /* Destroy a TSV */
    void thread_safe_var_destroy(thread_safe_var);

    /* Release the reference to the last value read by this thread from the TSV */
    void thread_safe_var_release(thread_safe_var);

    /* Wait for a value to be set on the TSV */
    int  thread_safe_var_wait(thread_safe_var);
```

Value version numbers increase monotonically when values are set.

# Why?  Because read-write locks are terrible

So you have rarely-changing typically-global data (e.g., loaded
configuration, plugin lists, ...), and you have many threads that read
this, and you want reads to be fast.  Worker threads need stable
configuration/whatever while doing work, then when they pick up another
task they can get a newer configuration if there is one.

How would one implement that?

A safe answer is: read-write locks around reading/writing the variable,
and reference count the data.

But read-write locks are inherently bad: readers either can starve
writers or can be blocked by writers.  Either way read-write locks are a
performance problem.

A "thread-safe variable", on the other hand, is always fast to read,
even when there's an active writer, and reading does not starve writers.

# How?

Two implementations are included at this time.

The two implementations have slightly different characteristics.

 - One implementation ("slot pair") has O(1) lock-less and spin-less
   reads and O(1) serialized writes.

   But readers call free() and the value destructor, and, sometimes have
   to signal a potentially-waiting writer, which involves acquiring a
   mutex -- a blocking operation, yes, though on an uncontended
   resource, so not really blocking.

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

 - The other implementation ("slot list") has O(1) lock-less reads, with
   unreferenced values garbage collected by serialized writers in `O(N
   log(M))` where N is the maximum number of live threads that have read
   the variable and M is the number of values that have been set and
   possibly released).  If writes are infrequent and readers make use of
   `thread_safe_var_release()`, then garbage collection is `O(1)`.
   
   Readers never call the allocator after the first read in any given
   thread, and writers never call the allocator while holding the writer
   lock.

   Readers have to loop over their fast path, a loop that could run
   indefinitely if there were infinitely many higher-priority writers
   who starve the reader of CPU time.  To help avoid this, writers yield
   the CPU before relinquishing the write lock, thus ensuring that some
   readers will have the CPU ahead of any awaiting higher-priority
   writers.

   This implementation has a list of referenced values, with the head of
   the list always being the current one, and a list of "subscription"
   slots, one slot per-reader thread.  Readers allocate a slot on first
   read, and thence copy the head of the values list to their slots.
   Writers have to perform garbage collection on the list of referenced
   values.

   Subscription slot allocation is lock-less.  Indeed, everything is
   lock-less in the reader, and unlike the slot-pair implementation
   there is no case where the reader has to acquire a lock to signal a
   writer.

   Values are released at the first write after the last reference is
   dropped, as values are garbage collected by writers.

The first implementation written was the slot-pair implementation.  The
slot-list design is much easier to understand on the read-side, but it
is significantly more complex on the write-side.

# Requirements

C89, POSIX threads (though TSV should be portable to Windows),
compilers with atomics intrinsics and/or atomics libraries.

In the future this may be upgraded to a C99 or even C11 requirement.

# Testing

A test program is included that hammers the implementation.  Run it in a
loop, with or without helgrind, TSAN (thread sanitizer), or other thread
race checkers, to look for data races.

Both implementations perform similarly well on the included test.

The test pits 20 reader threads waiting various small amounts of time
between reads (one not waiting at all), against 4 writer threads waiting
various small amounts of time between writes.  This test found a variety
of bugs during development.  In both cases writes are, on average, 5x
slower than reads, and reads are in the ten microseconds range on an old
laptop, running under virtualization.

# Performance

On an old i7 laptop, virtualized, reads on idle thread-safe variables
(i.e., no writers in sight) take about 15ns.  This is because the fast
path in both implementations consists of reading a thread-local variable
and then performing a single acquire-fenced memory read.

On that same system, when threads write very frequently then reads slow
down to about 8us (8000ns).  (But the test had eight times more threads
than CPUs, so the cost of context switching is included in that number.)

On that same system writes on a busy thread-safe variable take about
50us (50000ns), but non-contending writes on an otherwise idle
thread-safe variable take about 180ns.

I.e., this is blindingly fast, especially for intended use case
(infrequent writes).

# Install

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

 - `TSV_IMPLEMENTATION`

   Values: `-DUSE_TSV_SLOT_PAIR_DESIGN`, `-DUSE_TSV_SUBSCRIPTION_SLOTS_DESIGN`

 - `CPPDEFS`

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

# Thread Sanitizer (TSAN) Data Race Reports

Currently TSAN (GCC and Clang both) produces no reports.

It is trivial to cause TSAN to produce reports of data races by
replacing some atomic operations with non-atomic operations, therefore
it's clearly the case that TSAN works to find many data races.  That is
not proof that TSAN would catch all possible data races, or that the
tests exercise all possible data races.  A formal approach to proving
the correctness of TSVs would add value.

# Helgrind Data Race Reports

Currently Helgrind produces no race reports.  Using the
`ANNOTATE_HAPPENS_BEFORE()` and `ANNOTATE_HAPPENS_AFTER()` macros in
`<valgrind/helgrind.h>` provides Helgrind with the information it needs.

Use `make ... CPPDEFS=-DUSE_HELGRIND CSANFLAG=` to enable those macros.

> It is important to not use TSAN and Helgrind at the same time, as that
> makes Helgrind crash.  To disable TSAN set `CSANFLAG=` on the `make`
> command-line.

As with TSAN, it is trivial to make Helgrind report data races by not
using `CPPDEFS=-DUSE_HELGRIND` (but still using `CSANFLAG=`) then
running `helgrind ./t`, which then reports data races at places in the
source code that use atomic operations to... avoid data races.

# TODO

 - Don't create a pthread-specific variable for each TSV.  Instead share
   one pthread-specific for all TSVs.  This would require having the
   pthread-specific values be a pointer to a structure that has a
   pointer to an array of per-TSV elements, with
   `thread_safe_var_init()` allocating an array index for each TSV.

   This is important because there can be a maximum number of
   pthread-specifics and we must not be the cause of exceeding that
   maximum.

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
   be released?
  
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

 - Add a static initializer?

 - Add a better build system.

 - Add an implementation using read-write locks to compare performance
   with.

 - Use symbol names that don't conflict with any known atomics libraries
   (so those can be used as an atomics backend).  Currently the atomics
   symbols are loosely based on Illumos atomics primitives.

 - Support Win32 (perhaps by building a small pthread compatibility
   library; only mutexes and condition variables are needed).
