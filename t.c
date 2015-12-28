
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "thread_safe_global.h"
#include "atomics.h"

/*
 * TODO:
 *  - Gather and print performance numbers
 *
 *    In particular:
 *
 *     - start time
 *     - end time
 *     - number of reads done
 *     - number of reads / unit of time
 *     - estimated time spent sleeping vs. running for each writer
 *     - estimated time per write
 */

struct timespec
timeadd(struct timespec a, struct timespec b)
{
    struct timespec r;

    assert(a.tv_nsec >= 0 && a.tv_nsec < 1000000000);
    assert(b.tv_nsec >= 0 && b.tv_nsec < 1000000000);
    a.tv_nsec %= 1000000000;
    b.tv_nsec %= 1000000000;
    r.tv_sec = a.tv_sec + b.tv_sec;
    r.tv_nsec = a.tv_nsec + b.tv_nsec;
    if (r.tv_nsec > 1000000000) {
        r.tv_sec++;
        r.tv_nsec -= 1000000000;
    }
    assert(r.tv_nsec >= 0 && r.tv_nsec < 1000000000);
    return r;
}

struct timespec
timesub(struct timespec a, struct timespec b)
{
    struct timespec r;

    assert(a.tv_nsec >= 0 && a.tv_nsec < 1000000000);
    assert(b.tv_nsec >= 0 && b.tv_nsec < 1000000000);
    a.tv_nsec %= 1000000000;
    b.tv_nsec %= 1000000000;
    r.tv_sec = a.tv_sec - b.tv_sec;
    r.tv_nsec = a.tv_nsec - b.tv_nsec;
    if (r.tv_nsec < 0) {
        r.tv_sec--;
        r.tv_nsec += 1000000000;
    }
    assert(r.tv_nsec >= 0 && r.tv_nsec < 1000000000);
    return r;
}

static void *reader(void *data);
static void *writer(void *data);
static void dtor(void *);

static pthread_t readers[20];
static pthread_t writers[4];
#define NREADERS    (sizeof(readers)/sizeof(readers[0]))
#define NWRITERS    (sizeof(writers)/sizeof(writers[0]))
#define MY_NTHREADS (NREADERS + NWRITERS)
static pthread_mutex_t exit_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cv = PTHREAD_COND_INITIALIZER;
static uint32_t nthreads = MY_NTHREADS;
static uint32_t random_bytes[MY_NTHREADS];
static uint64_t *runs[MY_NTHREADS];
static struct timespec starttimes[MY_NTHREADS];
static struct timespec endtimes[MY_NTHREADS];
static struct timespec runtimes[MY_NTHREADS];
static struct timespec sleeptimes[MY_NTHREADS];

enum magic {
    MAGIC_FREED = 0xABADCAFEEFACDABAUL,
    MAGIC_INITED = 0xA600DA12DA1FFFFFUL,
    MAGIC_EXIT = 0xAABBCCDDFFEEDDCCUL,
};

pthread_var_np_t var;

int
main(int argc, char **argv)
{
    size_t i, k;
    int urandom_fd;
    uint64_t *magic_exit;
    uint64_t version;
    struct timespec starttime;
    struct timespec endtime;
    struct timespec runtime;
    struct timespec sleeptime;
    uint64_t rruns = 0;
    uint64_t wruns = 0;
    double usperrun;

    if ((magic_exit = malloc(sizeof(*magic_exit))) == NULL)
        err(1, "malloc failed");
    *magic_exit = MAGIC_EXIT;
    
    for (i = 0; i < MY_NTHREADS; i++)
        runs[i] = NULL;

    if ((errno = pthread_var_init_np(&var, dtor)) != 0)
        err(1, "pthread_var_init_np() failed");

    if ((urandom_fd = open("/dev/urandom", O_RDONLY)) == -1)
        err(1, "Failed to open(\"/dev/urandom\", O_RDONLY)");
    if (read(urandom_fd, random_bytes, sizeof(random_bytes)) != sizeof(random_bytes))
        err(1, "Failed to read() from /dev/urandom");
    (void) close(urandom_fd);

    if ((errno = pthread_mutex_lock(&exit_cv_lock)) != 0)
        err(1, "Failed to acquire exit lock");

    for (i = 0; i < NREADERS; i++) {
        if ((errno = pthread_create(&readers[i], NULL, reader, &random_bytes[i])) != 0)
            err(1, "Failed to create reader thread no. %ju", (uintmax_t)i);
        if ((errno = pthread_detach(readers[i])) != 0)
            err(1, "Failed to detach reader thread no. %ju", (uintmax_t)i);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &starttime) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    for (k = i, i = 0; i < NWRITERS; i++, k++) {
        if ((errno = pthread_create(&writers[i], NULL, writer, &random_bytes[k])) != 0)
            err(1, "Failed to create writer thread no. %ju", (uintmax_t)i);
        if ((errno = pthread_detach(writers[i])) != 0)
            err(1, "Failed to detach writer thread no. %ju", (uintmax_t)i);
    }

    while (atomic_cas_32(&nthreads, 0, 0) > 0) {
        if ((errno = pthread_cond_wait(&exit_cv, &exit_cv_lock)) != 0)
            err(1, "pthread_cond_wait(&exit_cv, &exit_cv_lock) failed");
        if (nthreads == NREADERS) {
            if ((errno = pthread_var_set_np(var, magic_exit, &version)) != 0)
                err(1, "pthread_var_set_np failed");
            printf("\nTold readers to exit.\n");
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &endtime) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    runtime = timesub(endtime, starttime);

    (void) pthread_mutex_unlock(&exit_cv_lock);
    pthread_var_destroy_np(var);

    printf("Run time: %jus, %juns\n", (uintmax_t)runtime.tv_sec,
           (uintmax_t)runtime.tv_nsec);

    runtime.tv_sec = 0;
    runtime.tv_nsec = 0;
    sleeptime.tv_sec = 0;
    sleeptime.tv_nsec = 0;
    for (i = 0; i < NREADERS; i++) {
        runtime = timeadd(runtime, runtimes[i]);
        sleeptime = timeadd(sleeptime, sleeptimes[i]);
        rruns += *(runs[i]);
    }
    printf("Read runs: %ju, read runtimes: %jus, %juns "
           "read sleeptimes: %jus, %juns\n",
           (uintmax_t)rruns,
           (uintmax_t)runtime.tv_sec / NREADERS,
           (uintmax_t)runtime.tv_nsec / NREADERS,
           (uintmax_t)sleeptime.tv_sec / NREADERS,
           (uintmax_t)sleeptime.tv_nsec / NREADERS);
    usperrun = (runtime.tv_sec * 1000000) / NREADERS +
               (runtime.tv_nsec / 1000) / NREADERS;
    usperrun /= rruns;
    printf("Average read time: %fus\n", usperrun);
    printf("Reads/s: %f/s\n", ((double)1000000.0)/usperrun);

    runtime.tv_sec = 0;
    runtime.tv_nsec = 0;
    sleeptime.tv_sec = 0;
    sleeptime.tv_nsec = 0;
    for (i = 0; i < NWRITERS; i++) {
        runtime = timeadd(runtime, runtimes[NREADERS + i]);
        sleeptime = timeadd(sleeptime, sleeptimes[NREADERS + i]);
        wruns += *(runs[NREADERS + i]);
    }
    printf("Write runs: %ju, write runtimes: %jus, %juns "
           "write sleeptimes: %jus, %juns\n",
           (uintmax_t)wruns,
           (uintmax_t)runtime.tv_sec / NWRITERS,
           (uintmax_t)runtime.tv_nsec / NWRITERS,
           (uintmax_t)sleeptime.tv_sec / NWRITERS,
           (uintmax_t)sleeptime.tv_nsec / NWRITERS);
    usperrun = (runtime.tv_sec * 1000000) / NWRITERS +
               (runtime.tv_nsec / 1000) / NWRITERS;
    usperrun /= wruns;
    printf("Average write time: %fus\n", usperrun);
    printf("Writes/s: %f/s\n", ((double)1000000.0)/usperrun);

    printf("\n\n");

    for (i = 0; i < MY_NTHREADS; i++)
        free(runs[i]);

    return 0;
}

static void *
reader(void *data)
{
    int thread_num = (uint32_t *)data - random_bytes;
    uint32_t i = *(uint32_t *)data;
    useconds_t us = i % 1000000;
    uint64_t version;
    uint64_t last_version = 0;
    uint64_t rruns = 0;
    void *p;

    runs[thread_num] = calloc(1, sizeof(runs[0]));

    if (us > 2000)
        us = 2000 + us % 2000;
    if (thread_num == 0 || thread_num == 1 || thread_num == 2)
        us = 0;
    if (thread_num == 19)
        us = 500000;

    printf("Reader (%jd) will sleep %uus between runs\n", (intmax_t)thread_num, us);

    if ((errno = pthread_var_wait_np(var)) != 0)
        err(1, "pthread_var_wait_np(var) failed");

    if (clock_gettime(CLOCK_MONOTONIC, &starttimes[thread_num]) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    for (;;) {
        assert(rruns == (*(runs[thread_num])));
        if ((errno = pthread_var_get_np(var, &p, &version)) != 0)
            err(1, "pthread_var_get_np(var) failed");

        if (version < last_version)
            err(1, "version went backwards for this reader!");
        last_version = version;
        assert(version == 0 || p != 0);
        if (*(uint64_t *)p == MAGIC_EXIT) {
            atomic_dec_32_nv(&nthreads);
            if ((errno = pthread_mutex_lock(&exit_cv_lock)) != 0)
                err(1, "Failed to acquire exit lock");
            if ((errno = pthread_cond_signal(&exit_cv)) != 0)
                err(1, "Failed to signal exit cv");
            if ((errno = pthread_mutex_unlock(&exit_cv_lock)) != 0)
                err(1, "Failed to release exit lock");
            break;
        }
        assert(*(uint64_t *)p != MAGIC_FREED);
        assert(*(uint64_t *)p == MAGIC_INITED);
        if (*(uint64_t *)p == MAGIC_FREED)
            err(1, "data is no longer live here!");
        if (*(uint64_t *)p != MAGIC_INITED)
            err(1, "data not valid here!");
        (*(runs[thread_num]))++;
        rruns++;
        if (rruns % 20 == 0 && us > 0)
            (void) write(1, ".", sizeof(".")-1);
        usleep(us);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &endtimes[thread_num]) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");
    assert(endtimes[thread_num].tv_sec != 0);

    sleeptimes[thread_num].tv_sec = (us * rruns) / 1000000;
    sleeptimes[thread_num].tv_nsec = ((us * rruns) % 1000000) * 1000;

    runtimes[thread_num] = timesub(endtimes[thread_num],
                                   starttimes[thread_num]);

    runtimes[thread_num] = timesub(runtimes[thread_num],
                                   sleeptimes[thread_num]);

    return NULL;
}

static void *
writer(void *data)
{
    int thread_num = (uint32_t *)data - random_bytes;
    uint32_t i = *(uint32_t *)data;
    useconds_t us = i % 1000000;
    uint64_t version;
    uint64_t last_version = 0;
    uint64_t wruns = 0;
    uint64_t *p;

    runs[thread_num] = calloc(1, sizeof(runs[0]));

    if (us > 9000)
        us = 9000 + us % 9000;

    i += i < 300 ? 300 : 0;
    if (i > 5000)
        i = 4999;

    if (thread_num == NREADERS + 3) {
        us %= 500;
        i *=10;
    }

    printf("Writer (%jd) will have %ju runs, sleeping %uus between\n", (intmax_t)thread_num - NREADERS, (uintmax_t)i, us);
    usleep(500000);

    if (clock_gettime(CLOCK_MONOTONIC, &starttimes[thread_num]) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    for (; i > 0; i--) {
        assert(wruns == (*(runs[thread_num])));
        if ((p = malloc(sizeof(*p))) == NULL)
            err(1, "malloc() failed");
        *p = MAGIC_INITED;
        if ((errno = pthread_var_set_np(var, p, &version)) != 0)
            err(1, "pthread_var_set_np(var) failed");
        if (version < last_version)
            err(1, "version went backwards for this writer!");
        last_version = version;
        (*(runs[thread_num]))++;
        wruns++;
        if (wruns % 5 == 0)
            (void) write(1, "-", sizeof("-")-1);
        usleep(us);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &endtimes[thread_num]) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    sleeptimes[thread_num].tv_sec = (us * wruns) / 1000000;
    sleeptimes[thread_num].tv_nsec = ((us * wruns) % 1000000) * 1000;

    runtimes[thread_num] = timesub(endtimes[thread_num],
                                   starttimes[thread_num]);

    runtimes[thread_num] = timesub(runtimes[thread_num],
                                   sleeptimes[thread_num]);

    /*atomic_dec_32_nv(&nthreads);*/
    printf("\nWriter (%jd) exiting; threads left: %u\n", (intmax_t)thread_num, atomic_dec_32_nv(&nthreads));
    if ((errno = pthread_mutex_lock(&exit_cv_lock)) != 0)
        err(1, "Failed to acquire exit lock");
    if ((errno = pthread_cond_signal(&exit_cv)) != 0)
        err(1, "Failed to signal exit cv");
    if ((errno = pthread_mutex_unlock(&exit_cv_lock)) != 0)
        err(1, "Failed to release exit lock");
    return NULL;
}

static void
dtor(void *data)
{
    *(uint64_t *)data = MAGIC_FREED;
    free(data);
}
