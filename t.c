
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
#include <unistd.h>
#include "thread_safe_global.h"

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

enum magic {
    MAGIC_FREED = 0xABADCAFEEFACDABAUL,
    MAGIC_INITED = 0xA600DA12DA1FFFFFUL,
    MAGIC_EXIT = 0xAABBCCDDFFEEDDCCUL,
};

pthread_cfvar_np_t var;

int
main(int argc, char **argv)
{
    size_t i, k;
    int urandom_fd;
    uint64_t magic_exit = MAGIC_EXIT;
    uint64_t version;
    
    for (i = 0; i < MY_NTHREADS; i++)
        runs[i] = 0;

    if ((errno = pthread_cfvar_init_np(&var, dtor)) != 0)
        err(1, "pthread_cfvar_init_np() failed");

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
            if ((errno = pthread_cfvar_set_np(&var, &magic_exit, &version)) != 0)
                err(1, "pthread_cfvar_set_np failed");
            printf("\nTold readers to exit.\n");
        }
    }
    (void) pthread_mutex_unlock(&exit_cv_lock);
    pthread_cfvar_destroy_np(&var);
    return 0;
}

static void *
reader(void *data)
{
    int thread_num = (uint32_t *)data - random_bytes;
    uint32_t i = *(uint32_t *)data;
    useconds_t us = i % 1000000;
    uint64_t version;
    static __thread uint64_t last_version = 0;
    static __thread uint64_t rruns = 0;
    void *p;

    runs[thread_num] = calloc(1, sizeof(runs[0]));

    if (us > 2000)
        us = 2000 + us % 2000;
    if (thread_num == 0 || thread_num == 1 || thread_num == 2)
        us = 0;
    if (thread_num == 19)
        us = 500000;

    printf("Reader (%jd) will sleep %uus between runs\n", (intmax_t)thread_num, us);

    if ((errno = pthread_cfvar_wait_np(&var)) != 0)
        err(1, "pthread_cfvar_wait_np(&var) failed");

    for (;;) {
        assert(rruns == (*(runs[thread_num])));
        if ((errno = pthread_cfvar_get_np(&var, &p, &version)) != 0)
            err(1, "pthread_cfvar_get_np(&var) failed");

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
            return NULL;
        }
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
    return NULL;
}

static void *
writer(void *data)
{
    int thread_num = (uint32_t *)data - random_bytes;
    uint32_t i = *(uint32_t *)data;
    useconds_t us = i % 1000000;
    uint64_t version;
    static __thread uint64_t last_version = 0;
    static __thread uint64_t wruns = 0;
    uint64_t *p;

    runs[thread_num] = calloc(1, sizeof(runs[0]));

    if (us > 9000)
        us = 9000 + us % 9000;

    i += i < 300 ? 300 : 0;
    if (i > 5000)
        i = 4999;

    if (thread_num == NREADERS + 3) {
        us = 500;
        i *=10;
    }

    printf("Writer (%jd) will have %ju runs, sleeping %uus between\n", (intmax_t)thread_num - NREADERS, (uintmax_t)i, us);
    usleep(500000);
    for (; i > 0; i--) {
        assert(wruns == (*(runs[thread_num])));
        if ((p = malloc(sizeof(*p))) == NULL)
            err(1, "malloc() failed");
        *p = MAGIC_INITED;
        if ((errno = pthread_cfvar_set_np(&var, p, &version)) != 0)
            err(1, "pthread_cfvar_set_np(&var) failed");
        if (version < last_version)
            err(1, "version went backwards for this reader!");
        last_version = version;
        (*(runs[thread_num]))++;
        wruns++;
        if (wruns % 5 == 0)
            (void) write(1, "-", sizeof("-")-1);
        usleep(us);
    }
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
