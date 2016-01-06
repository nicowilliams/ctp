
#ifndef TSGV_PRIV_H

#include <sys/types.h>
#include <stdint.h>
#include <pthread.h>
#include "thread_safe_global.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef pthread_var_destructor_np_t var_dtor_t;

/*
 * There are two designs.
 */
#if !defined(USE_TSGV_SLOT_PAIR_DESIGN) && !defined(USE_TSGV_SUBSCRIPTION_SLOTS_DESIGN)
#define USE_TSGV_SLOT_PAIR_DESIGN
#endif

#ifdef USE_TSGV_SLOT_PAIR_DESIGN

/*
 * Design #1: Two slots.
 *
 * This design uses a pair of "slots", such that one holds the current value of
 * the thread-safe global variable, while the other holds the previous/next
 * value.
 *
 * Writers make the previous slot into the new current slot, being careful not
 * to step on the toes of a reader that was reading from that slot thinking it
 * was the current slot.
 *
 * Readers are lock-less, except that when a reader is the last reader of a
 * slot it has to signal a writer that might be waiting for that reader to be
 * done with the slot.  Also, readers do call free(), which may acquire locks.
 * Sending that signal requires taking a lock that the writer will have dropped
 * in order to wait, thus it should be an uncontended lock, and if the reader
 * blocks racing with a writer, it should unblock very soon after.  This is
 * never needed when the value has not changed since the previous read.
 *
 * Both, reading, and writing are O(1).
 */

/*
 * Values set on a thread-global variable are wrapped with a struct that
 * holds a reference count.
 */
struct vwrapper {
    var_dtor_t          dtor;       /* value destructor */
    void                *ptr;       /* the actual value */
    uint32_t            nref;       /* release when drops to 0 */
    uint64_t            version;    /* version of this data */
};

/* This is a slot.  There are two of these. */
struct var {
    uint32_t            nreaders;   /* no. of readers active in this slot */
    struct vwrapper     *wrapper;   /* wraps real ptr, has nref */
    struct var          *other;     /* always points to the other slot */
    uint64_t            version;    /* version of this slot's data */
};

struct pthread_var_np {
    pthread_key_t       tkey;           /* to detect thread exits */
    pthread_mutex_t     write_lock;     /* one writer at a time */
    pthread_mutex_t     waiter_lock;    /* to signal waiters */
    pthread_cond_t      waiter_cv;      /* to signal waiters */
    pthread_mutex_t     cv_lock;        /* to signal waiting writer */
    pthread_cond_t      cv;             /* to signal waiting writer */
    struct var          vars[2];        /* the two slots */
    var_dtor_t          dtor;           /* both read this */
    uint64_t            next_version;   /* both read; writer writes */
};

#else

#ifndef USE_TSGV_SUBSCRIPTION_SLOTS_DESIGN
#error "wat"
#endif

/*
 * Design #2: Value list + per-reader thread slots.
 *
 * This design uses a list of referenced values and a set of slots, one per
 * thread that has read this thread-safe global variable.
 *
 * Readers "subscribe" the first time they read a thread-safe global variable,
 * allocating a slot.  Thereafter readers are very fast, using two fenced
 * memory operations to get the newest value of the thread-safe global
 * variable.
 *
 * Writers add new values to the head of a linked list, then garbage collect
 * the list by visiting all the reader subscription slots to mark the list then
 * sweep it.
 *
 * Readers never ever block and never call into the allocator.  First time
 * readers are O(N), else they are O(1).  Compare to the two-slot design where
 * readers may block briefly but are always O(1).
 *
 * Writers are serialized but do not block while holding the lock.  Writers do
 * not call the allocator while holding the lock.  Writers are O(N).  Compare to
 * the two-slot design, where writers are O(1).
 */

/* This is an element on the list of referenced values */
struct value {
    struct value        *next;          /* previous (still ref'd) value */
    void                *value;         /* actual value */
    uint64_t            version;        /* version number */
    uint32_t            referenced;     /* for mark and sweep */
};

/*
 * Each thread that has read this thread-safe global variable gets one
 * of these.
 */
struct slot {
    struct value        *value;         /* reference to last value read */
    uint32_t            in_use;         /* atomic */
    pthread_var_np_t    vp;             /* for cleanup from thread key dtor */
    /* We could add a pthread_t here */
};

/*
 * Slots are allocated in arrays that are linked into one larger logical
 * array.
 */
struct slots {
    struct slots        *next;          /* atomic */
    struct slot         *slot_array;    /* atomic */
    uint32_t            slot_count;     /* atomic */
    uint32_t            slot_base;      /* logical index of slot_array[0] */
};

struct pthread_var_np {
    pthread_key_t       tkey;           /* to detect thread exits */
    pthread_mutex_t     write_lock;     /* one writer at a time */
    pthread_mutex_t     waiter_lock;    /* to signal waiters */
    pthread_cond_t      waiter_cv;      /* to signal waiters */
    var_dtor_t          dtor;           /* value destructor */
    struct value        *values;        /* atomic ref'd value list head */
    struct slots        *slots;         /* atomic reader subscription slots */
    uint32_t            next_slot_idx;  /* atomic index of next new slot */
    uint32_t            slots_in_use;   /* atomic count of live readers */
    uint32_t            nvalues;        /* writer-only; for housekeeping */
};

#endif

#ifdef __cplusplus
}
#endif

#endif /* TSGV_PRIV_H */
