/*
 * FaultInjection.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "FaultInjection.hxx"

#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "cJSON.h"

int __real_open(const char *path, int flags, ...);
ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __real_read(int fd, void *buf, size_t n);
int __real_fsync(int fd);
int __real_rename(const char *from, const char *to);
int __real_mkdir(const char *path, mode_t mode);
int __real_chmod(const char *path, mode_t mode);
int __real_fchmod(int fd, mode_t mode);
int __real_unlink(const char *path);
int __real_pipe2(int fds[2], int flags);
int __real_socket(int domain, int type, int protocol);
int __real_bind(int fd, const struct sockaddr *addr, socklen_t len);
int __real_listen(int fd, int backlog);
int __real_pthread_create(pthread_t *t, const pthread_attr_t *a,
                          void *(*fn)(void *), void *arg);
time_t __real_time(time_t *t);

/* CppUTest's tracked allocation entry points (libCppUTest) */
void *cpputest_malloc_location(size_t size, const char *file, size_t line);
void *cpputest_calloc_location(size_t count, size_t size, const char *file,
                               size_t line);
void *cpputest_realloc_location(void *p, size_t size, const char *file,
                                size_t line);
void cpputest_free_location(void *p, const char *file, size_t line);
}

namespace
{
    struct Plan {
        int nth;        /* fail the nth call; 0 means no plan */
        int err;
        bool all;
        int count;
    };

    Plan _plans[Fault::NumCalls];
    std::string _filter;
    bool _haveFilter = false;
    long _timeOffset = 0;
    pthread_mutex_t _mutex = PTHREAD_MUTEX_INITIALIZER;

    int _jsonCount = 0;
    int _jsonFailAt = 0;
    int _allocCount = 0;
    int _allocFailAt = 0;

    /* true when this allocation must fail */
    bool
    allocShouldFail()
    {
        pthread_mutex_lock(&_mutex);
        _allocCount++;
        bool fail = (_allocFailAt > 0 && _allocCount == _allocFailAt);

        pthread_mutex_unlock(&_mutex);
        return fail;
    }

    /* true when this call must fail; sets errno */
    bool
    shouldFail(Fault::Call c, const char *path)
    {
        bool fail = false;

        pthread_mutex_lock(&_mutex);
        if (path != NULL && _haveFilter
            && strstr(path, _filter.c_str()) == NULL) {
            pthread_mutex_unlock(&_mutex);
            return false;
        }

        Plan &p = _plans[c];

        p.count++;
        if (p.all) {
            fail = true;
        } else if (p.nth > 0) {
            if (--p.nth == 0) {
                fail = true;
            }
        }

        int err = p.err;

        pthread_mutex_unlock(&_mutex);
        if (fail) {
            errno = err;
        }

        return fail;
    }

    void *
    jsonMalloc(size_t n)
    {
        pthread_mutex_lock(&_mutex);
        _jsonCount++;
        bool fail = (_jsonFailAt > 0 && _jsonCount == _jsonFailAt);
        pthread_mutex_unlock(&_mutex);
        return fail ? NULL : malloc(n);
    }

    void
    jsonFree(void *p)
    {
        free(p);
    }
}

void
Fault::reset()
{
    pthread_mutex_lock(&_mutex);
    memset(_plans, 0, sizeof(_plans));
    _haveFilter = false;
    _filter.clear();
    _timeOffset = 0;
    _jsonCount = 0;
    _jsonFailAt = 0;
    _allocCount = 0;
    _allocFailAt = 0;
    pthread_mutex_unlock(&_mutex);
}

void
Fault::failNth(Call c, int nth, int err)
{
    pthread_mutex_lock(&_mutex);
    _plans[c].nth = nth;
    _plans[c].err = err;
    _plans[c].all = false;
    pthread_mutex_unlock(&_mutex);
}

void
Fault::failAll(Call c, int err)
{
    pthread_mutex_lock(&_mutex);
    _plans[c].all = true;
    _plans[c].err = err;
    pthread_mutex_unlock(&_mutex);
}

void
Fault::onlyPathsContaining(const char *text)
{
    pthread_mutex_lock(&_mutex);
    if (text == NULL) {
        _haveFilter = false;
        _filter.clear();
    } else {
        _haveFilter = true;
        _filter = text;
    }

    pthread_mutex_unlock(&_mutex);
}

int
Fault::calls(Call c)
{
    pthread_mutex_lock(&_mutex);
    int n = _plans[c].count;

    pthread_mutex_unlock(&_mutex);
    return n;
}

void
Fault::advanceTime(long seconds)
{
    pthread_mutex_lock(&_mutex);
    _timeOffset = seconds;
    pthread_mutex_unlock(&_mutex);
}

void
Fault::failAllocNth(int nth)
{
    pthread_mutex_lock(&_mutex);
    _allocCount = 0;
    _allocFailAt = nth;
    pthread_mutex_unlock(&_mutex);
}

int
Fault::allocs()
{
    pthread_mutex_lock(&_mutex);
    int n = _allocCount;

    pthread_mutex_unlock(&_mutex);
    return n;
}

void
Fault::installJsonHooks()
{
    cJSON_Hooks hooks;

    hooks.malloc_fn = jsonMalloc;
    hooks.free_fn = jsonFree;
    cJSON_InitHooks(&hooks);
    _jsonCount = 0;
    _jsonFailAt = 0;
}

void
Fault::removeJsonHooks()
{
    cJSON_InitHooks(NULL);
    _jsonFailAt = 0;
}

void
Fault::failJsonAllocNth(int nth)
{
    pthread_mutex_lock(&_mutex);
    _jsonCount = 0;
    _jsonFailAt = nth;
    pthread_mutex_unlock(&_mutex);
}

int
Fault::jsonAllocs()
{
    pthread_mutex_lock(&_mutex);
    int n = _jsonCount;

    pthread_mutex_unlock(&_mutex);
    return n;
}

extern "C" {

int
__wrap_open(const char *path, int flags, ...)
{
    mode_t mode = 0;

    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;

        va_start(ap, flags);
        mode = (mode_t) va_arg(ap, int);
        va_end(ap);
    }

    if (shouldFail(Fault::Open, path)) {
        return -1;
    }

    return __real_open(path, flags, mode);
}

ssize_t
__wrap_write(int fd, const void *buf, size_t n)
{
    if (fd > 2 && shouldFail(Fault::Write, NULL)) {
        return -1;
    }

    return __real_write(fd, buf, n);
}

ssize_t
__wrap_read(int fd, void *buf, size_t n)
{
    if (fd > 2 && shouldFail(Fault::Read, NULL)) {
        return -1;
    }

    return __real_read(fd, buf, n);
}

int
__wrap_fsync(int fd)
{
    if (shouldFail(Fault::Fsync, NULL)) {
        return -1;
    }

    return __real_fsync(fd);
}

int
__wrap_rename(const char *from, const char *to)
{
    if (shouldFail(Fault::Rename, from)) {
        return -1;
    }

    return __real_rename(from, to);
}

int
__wrap_mkdir(const char *path, mode_t mode)
{
    if (shouldFail(Fault::Mkdir, path)) {
        return -1;
    }

    return __real_mkdir(path, mode);
}

int
__wrap_chmod(const char *path, mode_t mode)
{
    if (shouldFail(Fault::Chmod, path)) {
        return -1;
    }

    return __real_chmod(path, mode);
}

int
__wrap_fchmod(int fd, mode_t mode)
{
    if (shouldFail(Fault::Fchmod, NULL)) {
        return -1;
    }

    return __real_fchmod(fd, mode);
}

int
__wrap_unlink(const char *path)
{
    if (shouldFail(Fault::Unlink, path)) {
        return -1;
    }

    return __real_unlink(path);
}

int
__wrap_pipe2(int fds[2], int flags)
{
    if (shouldFail(Fault::Pipe2, NULL)) {
        return -1;
    }

    return __real_pipe2(fds, flags);
}

int
__wrap_socket(int domain, int type, int protocol)
{
    if (shouldFail(Fault::Socket, NULL)) {
        return -1;
    }

    return __real_socket(domain, type, protocol);
}

int
__wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    if (shouldFail(Fault::Bind, NULL)) {
        return -1;
    }

    return __real_bind(fd, addr, len);
}

int
__wrap_listen(int fd, int backlog)
{
    if (shouldFail(Fault::Listen, NULL)) {
        return -1;
    }

    return __real_listen(fd, backlog);
}

int
__wrap_pthread_create(pthread_t *t, const pthread_attr_t *a,
                      void *(*fn)(void *), void *arg)
{
    if (shouldFail(Fault::PthreadCreate, NULL)) {
        return errno;
    }

    return __real_pthread_create(t, a, fn, arg);
}

void *
test_alloc_malloc(size_t size, const char *file, int line)
{
    if (allocShouldFail()) {
        return NULL;
    }

    return cpputest_malloc_location(size, file, (size_t) line);
}

void *
test_alloc_calloc(size_t count, size_t size, const char *file, int line)
{
    if (allocShouldFail()) {
        return NULL;
    }

    return cpputest_calloc_location(count, size, file, (size_t) line);
}

void *
test_alloc_realloc(void *p, size_t size, const char *file, int line)
{
    if (allocShouldFail()) {
        return NULL;
    }

    return cpputest_realloc_location(p, size, file, (size_t) line);
}

void
test_alloc_free(void *p, const char *file, int line)
{
    cpputest_free_location(p, file, (size_t) line);
}

char *
test_alloc_strdup(const char *s, const char *file, int line)
{
    size_t n = strlen(s) + 1;
    char *p = (char *) test_alloc_malloc(n, file, line);

    if (p != NULL) {
        memcpy(p, s, n);
    }

    return p;
}

time_t
__wrap_time(time_t *t)
{
    time_t now = __real_time(NULL);

    pthread_mutex_lock(&_mutex);
    now += _timeOffset;
    pthread_mutex_unlock(&_mutex);
    if (t != NULL) {
        *t = now;
    }

    return now;
}

} /* extern "C" */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
