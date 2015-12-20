
#include <sys/types.h>
#include <sys/stat.h>
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
static pthread_t writers[3];
#define NREADERS    (sizeof(readers)/sizeof(readers[0]))
#define NWRITERS    (sizeof(writers)/sizeof(writers[0]))
#define MY_NTHREADS (NREADERS + NWRITERS)
static pthread_mutex_t exit_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t exit_cv = PTHREAD_COND_INITIALIZER;
static nthreads = MY_NTHREADS;
static uint32_t random_bytes[MY_NTHREADS];

#define MAGIC_FREED 0xABADCAFEEFACDABAUL
#define MAGIC_INITED 0xA600DA12DA1FFFFFUL

pthread_cfvar_np_t var;

int
main(int argc, char **argv)
{
    size_t i, k;
    int urandom_fd;

    if ((errno = pthread_cfvar_init_np(&var, dtor)) != 0)
        err(1, "pthread_cfvar_init_np() failed");

    if ((urandom_fd = open("/dev/urandom", O_RDONLY)) == -1)
        err(1, "Failed to open(\"/dev/urandom\", O_RDONLY)");
    if (read(urandom_fd, random_bytes, sizeof(random_bytes)) != sizeof(random_bytes))
        err(1, "Failed to read() from /dev/urandom");
    (void) close(urandom_fd);
    for (i = 0; i < sizeof(random_bytes); i++)
        random_bytes[i] += random_bytes[i] < 100 ? 100 : 0;

    if ((errno = pthread_mutex_lock(&exit_cv_lock)) != 0)
        err(1, "Failed to acquire exit lock");

    for (i = 0; i < sizeof(readers)/sizeof(readers[0]); i++) {
        if ((errno = pthread_create(&readers[i], NULL, reader, &random_bytes[i])) != 0)
            err(1, "Failed to create reader thread no. %ju", (uintmax_t)i);
        if ((errno = pthread_detach(readers[i])) != 0)
            err(1, "Failed to detach reader thread no. %ju", (uintmax_t)i);
    }
    for (k = i, i = 0; i < sizeof(writers)/sizeof(writers[0]); i++) {
        if ((errno = pthread_create(&writers[i], NULL, writer, &random_bytes[k])) != 0)
            err(1, "Failed to create writer thread no. %ju", (uintmax_t)i);
        if ((errno = pthread_detach(writers[i])) != 0)
            err(1, "Failed to detach writer thread no. %ju", (uintmax_t)i);
    }

    while (nthreads > 0) {
        if ((errno = pthread_cond_wait(&exit_cv, &exit_cv_lock)) != 0)
            err(1, "pthread_cond_wait(&exit_cv, &exit_cv_lock) failed");
    }
    (void) pthread_mutex_unlock(&exit_cv_lock);
    return 0;
}

static void *
reader(void *data)
{
    uint32_t i = *(uint32_t *)data;
    useconds_t us = i % 1000000;
    uint64_t version;
    static __thread uint64_t last_version = 0;

    if (us > 2000)
        us = 2000 + us % 2000;

    i += i < 1000 ? 1000 : 0;
    if (i > 10000)
        i = 99999;

    if ((errno = pthread_cfvar_wait_np(&var)) != 0)
        err(1, "pthread_cfvar_wait_np(&var) failed");

    for (; i > 0; i--) {
        if ((errno = pthread_cfvar_get_np(&var, &data, &version)) != 0)
            err(1, "pthread_cfvar_get_np(&var) failed");

        if (version < last_version)
            err(1, "version went backwards for this reader!");
        last_version = version;
        if (*(uint64_t *)data == MAGIC_FREED)
            err(1, "data is no longer live here!");
        if (*(uint64_t *)data != MAGIC_INITED)
            err(1, "data not valid here!");
        usleep(us);
    }
    return NULL;
}

static void *
writer(void *data)
{
    uint32_t i = *(uint32_t *)data;
    useconds_t us = i % 1000000;
    uint64_t version;
    static __thread uint64_t last_version = 0;

    if (us > 9000)
        us = 9000 + us % 9000;

    i += i < 300 ? 300 : 0;
    if (i > 50000)
        i = 49999;

    for (; i > 0; i--) {
        data = malloc(sizeof(*data));
        *(uint64_t *)data = MAGIC_INITED;
        if ((errno = pthread_cfvar_set_np(&var, data, NULL)) != 0)
            err(1, "pthread_cfvar_set_np(&var) failed");
        if (version < last_version)
            err(1, "version went backwards for this reader!");
        last_version = version;
        usleep(us);
    }
    return NULL;
}

static void
dtor(void *data)
{
    *(uint64_t *)data = MAGIC_FREED;
}
