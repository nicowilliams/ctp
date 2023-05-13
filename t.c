/*
 * Copyright (c) 2015 Cryptonector LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY
 * WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE 600
#define _DEFAULT_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "array_rope.h"
#include "desc_tbl.h"
#include "key.h"
#include "thread_safe_global.h"
#include "atomics.h"

#if !defined(USE_TSV_SLOT_PAIR_DESIGN) && !defined(USE_TSV_SUBSCRIPTION_SLOTS_DESIGN)
#define USE_TSV_SLOT_PAIR_DESIGN
#endif
#ifdef USE_TSV_SLOT_PAIR_DESIGN
#define TSV_TYPE "slotpair"
#endif
#ifdef USE_TSV_SUBSCRIPTION_SLOTS_DESIGN
#define TSV_TYPE "slotlist"
#endif

/*
 * TODO:
 *
 *  - Make most of main() into a utility function that takes a number of
 *    readers and a number of writers, and min/max sleep time for each
 *  - Make thread main functions take a pointer to a configuration
 *    struct instead of using globals all over
 *  - Make main() parse program arguments allowing the user to specify
 *    how many readers, how many writers, min/max sleep time for each
 *
 *    The idea is to allow the user to match the test to NCPUs to avoid
 *    context switching.  Perhaps there should be an option to use NCPU
 *    threads total and to bind each thread to a different CPU.
 */

static struct timespec
timeadd(struct timespec a, struct timespec b)
{
    struct timespec r;

    r.tv_sec = a.tv_sec + b.tv_sec +
        (a.tv_nsec + b.tv_nsec) / 1000000000;
    r.tv_nsec = (a.tv_nsec + b.tv_nsec) % 1000000000;
    return r;
}

static struct timespec
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
static void *idle_reader(void *);
static void *writer(void *data);
static void dtor(void *);

static pthread_t *readers;
static pthread_t *writers;
static size_t nreaders;
static size_t nwriters;
static size_t readerq; /* how often readers print on stdout */
static size_t writerq; /* how often writers print on stdout */
#define MY_NTHREADS (nreaders + nwriters)

static pthread_mutex_t exit_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cv = PTHREAD_COND_INITIALIZER;
static uint32_t nthreads;
static uint32_t *random_bytes;
static uint32_t *idleruns;
static uint64_t **runs;
static struct timespec *starttimes;
static struct timespec *endtimes;
static struct timespec *runtimes;
static struct timespec *sleeptimes;
static struct timespec *idleruntimes;

enum magic {
    MAGIC_FREED = 0xABADCAFEEFACDABAUL,
    MAGIC_INITED = 0xA600DA12DA1FFFFFUL,
    MAGIC_EXIT = 0xAABBCCDDFFEEDDCCUL,
};

thread_safe_var var;

static int
usage(const char *arg0, const char *arg, size_t nproc)
{
    int e = (arg != NULL && strcmp(arg, "-h") == 0) ? 0 : 1;
    FILE *f = e ? stderr : stdout;

    if (strchr(arg0, '/') != NULL)
        arg0 = strrchr(arg0, '/');

    fprintf(f, "Usage: %s [NREADERS [NWRITERS [READERQ [WRITERQ]]]]\n"
            "\n\tRuns NREADER and NWRITER threads racing on a single\n"
            "\tthread_safe_var.\n\n"
            "\tNREADERS defaults to %ju (NPROC).\n\n"
            "\tNWRITERS defaults to %ju (the greater of NREADERS / 5 or 1).\n"
            "\n\tEach thread will print a single character to stdout\n"
            "\tevery READERQ or WRITERQ runs, as appropriate.\n",
            arg0, (uintmax_t)nproc, (uintmax_t)(nproc / 5 ? nproc / 5 : 1));

    return e;
}

void
test_array_rope_1thread(void)
{
    array_rope a;
    void *vp;
    int ret, idx;

    if ((ret = array_rope_init(&a, sizeof(void *)))) abort();
    if (array_rope_getp(a, AR_GO_IF_SET, 0)) abort();
    if ((vp = array_rope_getp(a, AR_GO_FORCE, 0)) == NULL) abort();
    if (array_rope_getp(a, AR_GO_IF_SET, 1)) abort();
    if (array_rope_add(a, &vp, &idx)) abort();
    if (array_rope_getp(a, AR_GO_IF_SET, 1) == NULL) abort();
    if ((vp = array_rope_getp(a, AR_GO_FORCE, 7)) == NULL) abort();
    if (array_rope_get_index(a, vp) != 7) abort();
    if ((vp = array_rope_getp(a, AR_GO_FORCE, 10)) == NULL) abort();
    if (array_rope_get_index(a, vp) != 10) abort();
    if ((vp = array_rope_getp(a, AR_GO_FORCE, 51)) == NULL) abort();
    if (array_rope_get_index(a, vp) != 51) abort();
    if ((vp = array_rope_getp(a, AR_GO_FORCE, 1000)) == NULL) abort();
    if (array_rope_get_index(a, vp) != 1000) abort();
    array_rope_destroy(&a);
}

void
test_desc_tbl_1thread(void)
{
    desc_tbl t;
    int ret;

    if ((ret = desc_tbl_init(&t, NULL))) abort();
    desc_tbl_destroy(&t);
}

void
test_ctp_key_1thread(void)
{
    ctp_key k;
    int ret;

    if ((ret = ctp_key_create(&k, NULL))) abort();
    if ((ret = ctp_key_setspecific(k, &k))) abort();
    if (ctp_key_getspecific(k) != &k) abort();
    ctp_key_delete(&k);
}

int
main(int argc, char **argv)
{
    size_t i, k;
    size_t nproc = sysconf(_SC_NPROCESSORS_CONF) > 0 ?
                        sysconf(_SC_NPROCESSORS_CONF) : 20;
    int urandom_fd;
    uint64_t *magic_exit;
    uint64_t last_version;
    uint64_t version;
    struct timespec starttime;
    struct timespec endtime;
    struct timespec runtime;
    struct timespec sleeptime;
    uint64_t rruns;
    uint64_t wruns = 0;
    double usperrun;
    intmax_t n;
    ssize_t bytes;
    size_t arg = 0;
    char *e;

    test_array_rope_1thread();
    test_desc_tbl_1thread();
    test_ctp_key_1thread();

    if (argc >= 6)
        usage(argv[0], NULL, nproc);

    if (argc > 1) {
        errno = 0;
        if ((n = strtoimax(argv[++arg], &e, 10)) < 0 || n == INTMAX_MAX ||
            n >= 16384 || errno != 0 || e == NULL || *e != '\0')
            return usage(argv[0], argv[arg], nproc);
        nreaders = (size_t)n;
    }

    if (argc > 2) {
        errno = 0;
        if ((n = strtoimax(argv[++arg], &e, 10)) < 0 || n == INTMAX_MAX ||
            n >= 16384 || errno != 0 || e == NULL || *e != '\0')
            return usage(argv[0], argv[arg], nproc);
        nwriters = n;
    }

    if (argc > 3) {

        errno = 0;
        if ((n = strtoimax(argv[++arg], &e, 10)) < 0 || n == INTMAX_MAX ||
            n >= 16384 || errno != 0 || e == NULL || *e != '\0')
            return usage(argv[0], argv[arg], nproc);
        readerq = n;
    }

    if (argc > 4) {
        errno = 0;
        if ((n = strtoimax(argv[++arg], &e, 10)) < 0 || n == INTMAX_MAX ||
            n >= 16384 || errno != 0 || e == NULL || *e != '\0')
            return usage(argv[0], argv[arg], nproc);
        writerq = n;
    }

    if (nreaders == 0)
        nreaders = nproc;
    if (nwriters == 0)
        nwriters = nreaders / 5 > 0 ? nreaders / 5 : 1;

    if (readerq == 0 && (readerq = nreaders / 10) < 20)
        readerq = 20;
    if (writerq == 0 && (writerq = nwriters / 10) < 20)
        writerq = 20;
    nthreads = MY_NTHREADS;

    printf("Testing the thread-safe variable implementation type \"%s\"\n",
           TSV_TYPE);
    printf("Will use %ju reader threads and %ju writer threads\n",
           (uintmax_t)nreaders, (uintmax_t)nwriters);
    printf("Readers will print every %ju runs\n", (uintmax_t)readerq);
    printf("Writers will print every %ju runs\n", (uintmax_t)writerq);
    sleep(1);

#define MY_CALLOC1(v, n) (((v) = calloc((n), sizeof((v)[0]))) == NULL)

    if (MY_CALLOC1(readers, nreaders) ||
        MY_CALLOC1(writers, nwriters) ||
        MY_CALLOC1(random_bytes, MY_NTHREADS) ||
        MY_CALLOC1(idleruns, MY_NTHREADS) ||
        MY_CALLOC1(runs, MY_NTHREADS) ||
        MY_CALLOC1(starttimes, MY_NTHREADS) ||
        MY_CALLOC1(endtimes, MY_NTHREADS) ||
        MY_CALLOC1(runtimes, MY_NTHREADS) ||
        MY_CALLOC1(sleeptimes, MY_NTHREADS) ||
        MY_CALLOC1(idleruntimes, MY_NTHREADS))
        err(1, "calloc failed");

    if ((magic_exit = malloc(sizeof(*magic_exit))) == NULL)
        err(1, "malloc failed");
    *magic_exit = MAGIC_EXIT;
    
    for (i = 0; i < MY_NTHREADS; i++)
        runs[i] = NULL;

    if ((errno = thread_safe_var_init(&var, dtor)) != 0)
        err(1, "thread_safe_var_init() failed");

    if ((urandom_fd = open("/dev/urandom", O_RDONLY)) == -1)
        err(1, "Failed to open(\"/dev/urandom\", O_RDONLY)");
    if ((bytes = read(urandom_fd, random_bytes,
             sizeof(random_bytes[0]) * MY_NTHREADS)) < 0)
        err(1, "Failed to read() from /dev/urandom");
    if ((size_t)bytes != sizeof(random_bytes[0]) * MY_NTHREADS)
        err(1, "Failed to read() enough from /dev/urandom");
    (void) close(urandom_fd);

    if ((errno = pthread_mutex_lock(&exit_cv_lock)) != 0)
        err(1, "Failed to acquire exit lock");

    for (i = 0; i < nreaders; i++) {
        if ((errno = pthread_create(&readers[i], NULL, reader, &random_bytes[i])) != 0)
            err(1, "Failed to create reader thread no. %ju", (uintmax_t)i);
        if ((errno = pthread_detach(readers[i])) != 0)
            err(1, "Failed to detach reader thread no. %ju", (uintmax_t)i);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &starttime) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    for (k = i, i = 0; i < nwriters; i++, k++) {
        if ((errno = pthread_create(&writers[i], NULL, writer, &random_bytes[k])) != 0)
            err(1, "Failed to create writer thread no. %ju", (uintmax_t)i);
        if ((errno = pthread_detach(writers[i])) != 0)
            err(1, "Failed to detach writer thread no. %ju", (uintmax_t)i);
    }

    while (atomic_cas_32(&nthreads, 0, 0) > 0) {
        if ((errno = pthread_cond_wait(&exit_cv, &exit_cv_lock)) != 0)
            err(1, "pthread_cond_wait(&exit_cv, &exit_cv_lock) failed");
        if (atomic_cas_32(&nthreads, 0, 0) == nreaders) {
            if ((errno = thread_safe_var_set(var, magic_exit, &last_version)) != 0)
                err(1, "thread_safe_var_set() failed");
            printf("\nTold readers to exit.\n");
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &endtime) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    runtime = timesub(endtime, starttime);

    {
        void *p;
        struct timespec idle_start;
        struct timespec idle_end;
        struct timespec idle_run;

#define IDLE_READ_RUNS 50000

        /* Measure single-threaded read performance on an idle var */
        if ((errno = thread_safe_var_get(var, &p, &version)) != 0)
            err(1, "thread_safe_var_get() failed");
        assert(version == last_version);
        if (clock_gettime(CLOCK_MONOTONIC, &idle_start) != 0)
            err(1, "clock_gettime(CLOCK_MONOTONIC) failed");
        for (i = 0; i < IDLE_READ_RUNS; i++) {
            if ((errno = thread_safe_var_get(var, &p, &version)) != 0)
                err(1, "thread_safe_var_get() failed");
            assert(version == last_version);
        }
        if (clock_gettime(CLOCK_MONOTONIC, &idle_end) != 0)
            err(1, "clock_gettime(CLOCK_MONOTONIC) failed");
        idle_run = timesub(idle_end, idle_start);
        usperrun = idle_run.tv_sec * 1000000 + idle_run.tv_nsec / 1000;
        usperrun /= IDLE_READ_RUNS;
        printf("Reads on idle var: %fus/read, %f reads/s\n",
               usperrun, ((double)1000000.0)/usperrun);

#define THREADED_IDLE_READ_RUNS 50000

        /* Test threaded idle reader performance */
        atomic_cas_32(&nthreads, 0, nreaders);
        for (i = 0; i < nreaders; i++) {
            idleruns[i] = THREADED_IDLE_READ_RUNS;
            if ((errno = pthread_create(&readers[i], NULL, idle_reader,
                                        idleruns)) != 0)
                err(1, "Failed to create reader thread no. %ju", (uintmax_t)i);
            if ((errno = pthread_detach(readers[i])) != 0)
                err(1, "Failed to detach reader thread no. %ju", (uintmax_t)i);
        }

        while (atomic_cas_32(&nthreads, 0, 0) > 0) {
            if ((errno = pthread_cond_wait(&exit_cv, &exit_cv_lock)) != 0)
                err(1, "pthread_cond_wait(&exit_cv, &exit_cv_lock) failed");
            if (atomic_cas_32(&nthreads, 0, 0) == nreaders) {
                if ((errno = thread_safe_var_set(var, magic_exit, &last_version)) != 0)
                    err(1, "thread_safe_var_set() failed");
                printf("\nTold readers to exit.\n");
            }
        }

        rruns = 0;
        runtime.tv_sec = 0;
        runtime.tv_nsec = 0;
        for (i = 0; i < nreaders; i++) {
            runtime = timeadd(runtime, idleruntimes[i]);
            rruns += idleruns[i];
        }
        printf("Threaded idle read runs: %ju, read runtimes: %jus, %juns\n",
               (uintmax_t)rruns,
               (uintmax_t)runtime.tv_sec / nreaders,
               (uintmax_t)runtime.tv_nsec / nreaders);
        usperrun = (runtime.tv_sec * 1000000) / nreaders +
                   (runtime.tv_nsec / 1000) / nreaders;
        usperrun /= rruns;
        printf("Average threaded idle read time: %fus\n", usperrun);
        printf("Threaded idle reads/s: %f/s\n", ((double)1000000.0)/usperrun);

#define IDLE_WRITE_RUNS 5000

        /* Measure single-threaded write performance on an idle var */
        if (clock_gettime(CLOCK_MONOTONIC, &idle_start) != 0)
            err(1, "clock_gettime(CLOCK_MONOTONIC) failed");
        for (i = 0; i < IDLE_READ_RUNS; i++) {
            if ((errno = thread_safe_var_set(var, (void *)0x08UL, &version)) != 0)
                err(1, "thread_safe_var_set() failed");
            assert(version == last_version + 1);
            last_version = version;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &idle_end) != 0)
            err(1, "clock_gettime(CLOCK_MONOTONIC) failed");
        idle_run = timesub(idle_end, idle_start);
        usperrun = idle_run.tv_sec * 1000000 + idle_run.tv_nsec / 1000;
        usperrun /= IDLE_READ_RUNS;
        printf("Writes on idle var: %fus/write, %f writes/s\n",
               usperrun, ((double)1000000.0)/usperrun);
    }

    (void) pthread_mutex_unlock(&exit_cv_lock);
    thread_safe_var_destroy(var);

    printf("Run time: %jus, %juns\n", (uintmax_t)runtime.tv_sec,
           (uintmax_t)runtime.tv_nsec);

    rruns = 0;
    runtime.tv_sec = 0;
    runtime.tv_nsec = 0;
    sleeptime.tv_sec = 0;
    sleeptime.tv_nsec = 0;
    for (i = 0; i < nreaders; i++) {
        runtime = timeadd(runtime, runtimes[i]);
        sleeptime = timeadd(sleeptime, sleeptimes[i]);
        rruns += *(runs[i]);
    }
    printf("Read runs: %ju, read runtimes: %jus, %juns "
           "read sleeptimes: %jus, %juns\n",
           (uintmax_t)rruns,
           (uintmax_t)runtime.tv_sec / nreaders,
           (uintmax_t)runtime.tv_nsec / nreaders,
           (uintmax_t)sleeptime.tv_sec / nreaders,
           (uintmax_t)sleeptime.tv_nsec / nreaders);
    usperrun = (runtime.tv_sec * 1000000) / nreaders +
               (runtime.tv_nsec / 1000) / nreaders;
    usperrun /= rruns;
    printf("Average read time: %fus\n", usperrun);
    printf("Reads/s: %f/s\n", ((double)1000000.0)/usperrun);

    runtime.tv_sec = 0;
    runtime.tv_nsec = 0;
    sleeptime.tv_sec = 0;
    sleeptime.tv_nsec = 0;
    for (i = 0; i < nwriters; i++) {
        runtime = timeadd(runtime, runtimes[nreaders + i]);
        sleeptime = timeadd(sleeptime, sleeptimes[nreaders + i]);
        wruns += *(runs[nreaders + i]);
    }
    printf("Write runs: %ju, write runtimes: %jus, %juns "
           "write sleeptimes: %jus, %juns\n",
           (uintmax_t)wruns,
           (uintmax_t)runtime.tv_sec / nwriters,
           (uintmax_t)runtime.tv_nsec / nwriters,
           (uintmax_t)sleeptime.tv_sec / nwriters,
           (uintmax_t)sleeptime.tv_nsec / nwriters);
    usperrun = (runtime.tv_sec * 1000000) / nwriters +
               (runtime.tv_nsec / 1000) / nwriters;
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
    int first = 1;
    void *p;

    thread_num %= MY_NTHREADS;
    runs[thread_num] = calloc(1, sizeof(runs[0]));

    if (us > 2000)
        us = 2000 + us % 2000;
    if (thread_num == 0 || thread_num == 1 || thread_num == 2)
        us = 0; /* A few fast threads */
    if (thread_num == (int)nreaders - 1)
        us = 500000; /* One really slow thread */

    printf("Reader (%jd) will sleep %uus between runs\n", (intmax_t)thread_num, us);

    if ((errno = thread_safe_var_wait(var)) != 0)
        err(1, "thread_safe_var_wait() failed");

    if (clock_gettime(CLOCK_MONOTONIC, &starttimes[thread_num]) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    for (;;) {
        assert(rruns == (*(runs[thread_num])));
        if ((errno = thread_safe_var_get(var, &p, &version)) != 0)
            err(1, "thread_safe_var_get() failed");

        if (version < last_version)
            err(1, "version went backwards for this reader! "
                "new version is %jd, previous is %jd",
                version, last_version);
        last_version = version;
        assert(version == 0 || p != 0);
        if (*(uint64_t *)p == MAGIC_EXIT)
            break;
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

        if (first) {
            printf("(%d)", thread_num);
            fflush(stdout);
            first = 0;
        }
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

    atomic_dec_32_nv(&nthreads);
    if ((errno = pthread_mutex_lock(&exit_cv_lock)) != 0)
        err(1, "Failed to acquire exit lock");
    if ((errno = pthread_cond_signal(&exit_cv)) != 0)
        err(1, "Failed to signal exit cv");
    if ((errno = pthread_mutex_unlock(&exit_cv_lock)) != 0)
        err(1, "Failed to release exit lock");

    return NULL;
}

static void *
idle_reader(void *data)
{
    int thread_num = (uint32_t *)data - idleruns;
    struct timespec start, end, t;
    uint64_t version;
    uint64_t i;
    void *p;
    int first = 1;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    for (i = idleruns[thread_num]; i > 0; i--) {
        if ((errno = thread_safe_var_get(var, &p, &version)) != 0)
            err(1, "thread_safe_var_get() failed");

        if (first) {
            printf("(%d)", thread_num);
            fflush(stdout);
            first = 0;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    t = timesub(end, start);
    if (sizeof(t.tv_sec) == 4)
        atomic_write_32((volatile uint32_t *)&idleruntimes[thread_num].tv_sec, t.tv_sec);
    else if (sizeof(t.tv_sec) == 8)
        atomic_write_64((volatile uint64_t *)&idleruntimes[thread_num].tv_sec, t.tv_sec);
    else
        abort();
    if (sizeof(t.tv_nsec) == 4)
        atomic_write_32((volatile uint32_t *)&idleruntimes[thread_num].tv_nsec, t.tv_nsec);
    else if (sizeof(t.tv_sec) == 8)
        atomic_write_64((volatile uint64_t *)&idleruntimes[thread_num].tv_nsec, t.tv_nsec);
    else
        abort();

    atomic_dec_32_nv(&nthreads);
    if ((errno = pthread_mutex_lock(&exit_cv_lock)) != 0)
        err(1, "Failed to acquire exit lock");
    if ((errno = pthread_cond_signal(&exit_cv)) != 0)
        err(1, "Failed to signal exit cv");
    if ((errno = pthread_mutex_unlock(&exit_cv_lock)) != 0)
        err(1, "Failed to release exit lock");

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

    if (thread_num - nreaders == nwriters - 1) {
        us %= 500;
        i *=10;
    }

    printf("Writer (%jd) will have %ju runs, sleeping %uus between\n", (intmax_t)thread_num - nreaders, (uintmax_t)i, us);
    usleep(500000);

    if (clock_gettime(CLOCK_MONOTONIC, &starttimes[thread_num]) != 0)
        err(1, "clock_gettime(CLOCK_MONOTONIC) failed");

    for (; i > 0; i--) {
        assert(wruns == (*(runs[thread_num])));
        if ((p = malloc(sizeof(*p))) == NULL)
            err(1, "malloc() failed");
        *p = MAGIC_INITED;
        if ((errno = thread_safe_var_set(var, p, &version)) != 0)
            err(1, "thread_safe_var_set() failed");
        if (version < last_version)
            err(1, "version went backwards for this writer! "
                "new version is %jd, previous is %jd",
                version, last_version);
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
    if (data == (void *)0x08UL)
        return;
    *(uint64_t *)data = MAGIC_FREED;
    free(data);
}
