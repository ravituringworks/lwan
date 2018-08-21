/*
 * lwan - simple web server
 * Copyright (c) 2012 Leandro A. F. Pereira <leandro@hardinfo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "lwan.h"

#ifndef HAVE_PTHREADBARRIER
#define PTHREAD_BARRIER_SERIAL_THREAD -1

int
pthread_barrier_init(pthread_barrier_t *restrict barrier,
        const pthread_barrierattr_t *restrict attr __attribute__((unused)),
        unsigned int count) {
    if (count == 0) {
        return -1;
    }

    barrier->count = count;
    barrier->in = 0;

    if (pthread_mutex_init(&barrier->mutex, NULL) < 0)
        return -1;

    if (pthread_cond_init(&barrier->cond, NULL) < 0) {
        pthread_mutex_destroy(&barrier->mutex);
        return -1;
    }

    return 0;
}

int
pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    barrier->in = 0;

    return 0;
}

int
pthread_barrier_wait(pthread_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);

    barrier->in++;
    if (barrier->in >= barrier->count) {
        barrier->in = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);

        return PTHREAD_BARRIER_SERIAL_THREAD;
    }

    pthread_cond_wait(&barrier->cond, &barrier->mutex);
    pthread_mutex_unlock(&barrier->mutex);

    return 0;
}
#endif

#ifndef HAVE_MEMPCPY
void *
mempcpy(void *dest, const void *src, size_t len)
{
    char *p = memcpy(dest, src, len);
    return p + len;
}
#endif

#ifndef HAVE_MEMRCHR
void *
memrchr(const void *s, int c, size_t n)
{
    const char *end = (const char *)s + n + 1;
    const char *prev = NULL;

    for (const char *cur = s; cur <= end; prev = cur++) {
        cur = (const char *)memchr(cur, c, (size_t)(ptrdiff_t)(end - cur));
        if (!cur)
            break;
    }

    return (void *)prev;
}
#endif

#ifndef HAVE_PIPE2
int
pipe2(int pipefd[2], int flags)
{
   int r;

   r = pipe(pipefd);
   if (r < 0)
      return r;

   if (fcntl(pipefd[0], F_SETFL, flags) < 0 || fcntl(pipefd[1], F_SETFL, flags) < 0) {
      int saved_errno = errno;

      close(pipefd[0]);
      close(pipefd[1]);

      errno = saved_errno;
      return -1;
   }

   return 0;
}
#endif

#ifndef HAVE_ACCEPT4
int
accept4(int sock, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
   int fd = accept(sock, addr, addrlen);
   int newflags = 0;

   if (fd < 0)
       return fd;

   if (flags & SOCK_NONBLOCK) {
       newflags |= O_NONBLOCK;
       flags &= ~SOCK_NONBLOCK;
   }
   if (flags & SOCK_CLOEXEC) {
       newflags |= O_CLOEXEC;
       flags &= ~SOCK_CLOEXEC;
   }
   if (flags) {
       errno = -EINVAL;
       return -1;
   }

   if (fcntl(fd, F_SETFL, newflags) < 0) {
       int saved_errno = errno;

       close(fd);

       errno = saved_errno;
       return -1;
   }

   return fd;
}
#endif

#ifndef HAVE_CLOCK_GETTIME
int
clock_gettime(clockid_t clk_id, struct timespec *ts)
{
   switch (clk_id) {
   case CLOCK_MONOTONIC:
   case CLOCK_MONOTONIC_COARSE:
       /* FIXME: time() isn't monotonic */
       ts->tv_sec = time(NULL);
       ts->tv_nsec = 0;
       return 0;
   }

   errno = EINVAL;
   return -1;
}
#endif

#if !defined(HAVE_EPOLL) && defined(HAVE_KQUEUE)
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include "hash.h"

int
epoll_create1(int flags __attribute__((unused)))
{
    return kqueue();
}

int
epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    struct kevent ev;

    switch (op) {
    case EPOLL_CTL_ADD:
    case EPOLL_CTL_MOD: {
        int events = 0;
        /* EV_CLEAR should be set only if EPOLLET is there, but Lwan doesn't
         * always set EPOLLET.  In the meantime, force EV_CLEAR every time.  */
        uintptr_t udata = (uintptr_t)event->data.ptr;
        int flags = EV_ADD | EV_CLEAR;

        if (event->events & EPOLLIN) {
            events = EVFILT_READ;
        } else if (event->events & EPOLLOUT) {
            events = EVFILT_WRITE;
        } else {
            events = EVFILT_WRITE;
            udata = 1;
        }

        if (event->events & EPOLLONESHOT)
            flags |= EV_ONESHOT;
        if (event->events & EPOLLRDHUP)
            flags |= EV_EOF;
        if (event->events & EPOLLERR)
            flags |= EV_ERROR;

        EV_SET(&ev, fd, events, flags, 0, 0, (void *)udata);
        break;
    }

    case EPOLL_CTL_DEL:
        EV_SET(&ev, fd, 0, EV_DELETE, 0, 0, 0);
        break;

    default:
        errno = EINVAL;
        return -1;
    }

    return kevent(epfd, &ev, 1, NULL, 0, NULL);
}

static struct timespec *
to_timespec(struct timespec *t, int ms)
{
    if (ms < 0)
        return NULL;

    t->tv_sec = ms / 1000;
    t->tv_nsec = (ms % 1000) * 1000000;

    return t;
}

int
epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    struct epoll_event *ev = events;
    struct kevent evs[maxevents];
    struct timespec tmspec;
    struct hash *coalesce;
    int i, r;

    coalesce = hash_int_new(NULL, NULL);
    if (UNLIKELY(!coalesce))
        return -1;

    r = kevent(epfd, NULL, 0, evs, maxevents, to_timespec(&tmspec, timeout));
    if (UNLIKELY(r < 0)) {
        hash_free(coalesce);
        return -1;
    }

    for (i = 0; i < r; i++) {
        struct kevent *kev = &evs[i];
        uint32_t mask = (uint32_t)(uintptr_t)hash_find(coalesce,
            (void*)(intptr_t)evs[i].ident);

        if (kev->flags & EV_ERROR)
            mask |= EPOLLERR;
        if (kev->flags & EV_EOF)
            mask |= EPOLLRDHUP;

        if (kev->filter == EVFILT_READ)
            mask |= EPOLLIN;
        else if (kev->filter == EVFILT_WRITE)
            mask |= EPOLLOUT;

        hash_add(coalesce, (void*)(intptr_t)evs[i].ident, (void *)(uintptr_t)mask);
    }

    for (i = 0; i < r; i++) {
        uintptr_t udata = (uintptr_t)evs[i].udata;
        void *maskptr;

        if (udata == 1) {
            continue;
        }

        maskptr = hash_find(coalesce, (void*)(intptr_t)evs[i].ident);
        if (maskptr) {
            struct kevent *kev = &evs[i];

            ev->data.ptr = kev->udata;
            ev->events = (uint32_t)(uintptr_t)maskptr;
            ev++;
        }
    }

    hash_free(coalesce);
    return (int)(intptr_t)(ev - events);
}
#elif !defined(HAVE_EPOLL)
#error epoll() not implemented for this platform
#endif

#if defined(__linux__) || defined(__CYGWIN__)
#if defined(HAVE_GETAUXVAL)
#include <sys/auxv.h>
#endif

int
proc_pidpath(pid_t pid, void *buffer, size_t buffersize)
{
    ssize_t path_len;

    if (getpid() != pid) {
        errno = EACCES;

        return -1;
    }

#if defined(HAVE_GETAUXVAL)
    const char *execfn = (const char *)getauxval(AT_EXECFN);

    if (execfn) {
        size_t len = strlen(execfn);

        if (len + 1 < buffersize) {
            memcpy(buffer, execfn, len + 1);

            return 0;
        }
    }
#endif

    path_len = readlink("/proc/self/exe", buffer, buffersize);
    if (path_len < 0)
        return -1;

    if (path_len < (ssize_t)buffersize) {
        ((char *)buffer)[path_len] = '\0';

        return 0;
    }

    errno = EOVERFLOW;
    return -1;
}

#elif defined(__FreeBSD__)
#include <sys/sysctl.h>

int
proc_pidpath(pid_t pid, void *buffer, size_t buffersize)
{
    int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t path_len = buffersize;

    if (getpid() != pid) {
        errno = EACCES;

        return -1;
    }

    if (sysctl(mib, N_ELEMENTS(mib), buffer, &path_len, NULL, 0) < 0)
        return -1;

    return 0;
}
#elif defined(HAVE_DLADDR) && !defined(__APPLE__)
#include <dlfcn.h>

int
proc_pidpath(pid_t pid, void *buffer, size_t buffersize)
{
    Dl_info info;

    if (getpid() != pid) {
        errno = EACCES;
        return -1;
    }

    extern int main();
    if (dladdr(main, &info)) {
        if (!info.dli_fname)
            goto fallback;

        if (buffersize < PATH_MAX - 1)
            goto fallback;

        if (realpath(info.dli_fname, buffer))
            return 0;
    }

fallback:
    if (strlcpy(buffer, "lwan", buffersize) >= buffersize) {
        errno = ENOMEM;
        return -1;
    }

    return 0;
}
#elif !defined(__APPLE__)
#error proc_pidpath() not implemented for this architecture
#endif

#if defined(__linux__)
#include <sys/syscall.h>

long
gettid(void)
{
    return syscall(SYS_gettid);
}
#elif defined(__FreeBSD__)
#include <sys/thr.h>

long
gettid(void)
{
    long ret;

    thr_self(&ret);

    return ret;
}
#elif defined(__APPLE__) && MAC_OS_X_VERSION_MAX_ALLOWED >= 101200
#include <sys/syscall.h>

long
gettid(void)
{
    return syscall(SYS_thread_selfid);
}
#else
long
gettid(void)
{
    return (long)pthread_self();
}
#endif

#if defined(__APPLE__)
/* NOTE: Although saved UID/GID cannot be set using sysctl(), for the use
 * case in Lwan, it's possible to obtain the value and check if they're the
 * ones expected -- and abort if it's not.  Should be good enough for a
 * wrapper like this.  */

#include <sys/sysctl.h>

static int
get_current_proc_info(struct kinfo_proc *kp)
{
    int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    size_t len = sizeof(*kp);

    return sysctl(mib, N_ELEMENTS(mib), kp, &len, NULL, 0);
}

int
getresuid(uid_t *ruid, uid_t *euid, uid_t *suid)
{
    struct kinfo_proc kp;

    if (!get_current_proc_info(&kp)) {
        *ruid = getuid();
        *euid = geteuid();
        *suid = kp.kp_eproc.e_pcred.p_svuid;

        return 0;
    }

    return -1;
}

int
setresuid(uid_t ruid, uid_t euid, uid_t suid __attribute__((unused)))
{
    return setreuid(ruid, euid);
}

int
setresgid(gid_t rgid, gid_t egid, gid_t sgid __attribute__((unused)))
{
    return setregid(rgid, egid);
}

int
getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid)
{
    struct kinfo_proc kp;

    if (!get_current_proc_info(&kp)) {
        *rgid = getgid();
        *egid = getegid();
        *sgid = kp.kp_eproc.e_pcred.p_svgid;

        return 0;
    }

    return -1;
}
#endif

#if !defined(HAVE_MKOSTEMP)
int mkostemp(char *tmpl, int flags)
{
    int fd, fl;

    fd = mkstemp(tmpl);
    if (fd < 0)
        return -1;

    fl = fcntl(fd, F_GETFD);
    if (fl < 0)
        goto out;

    if (flags & O_CLOEXEC)
        fl |= FD_CLOEXEC;

    if (fcntl(fd, F_SETFD, fl) < 0)
        goto out;

    return fd;

out:
    close(fd);
    return -1;
}
#endif

#if !defined(HAVE_RAWMEMCHR)
void *rawmemchr(const void *ptr, char c)
{
    return memchr(ptr, c, SIZE_MAX);
}
#endif

#if !defined (HAVE_REALLOCARRAY)
/*	$OpenBSD: reallocarray.c,v 1.2 2014/12/08 03:45:00 bcook Exp $	*/
/*
 * Copyright (c) 2008 Otto Moerbeek <otto@drijf.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#if !defined(HAVE_BUILTIN_MUL_OVERFLOW)
/*
 * This is sqrt(SIZE_MAX+1), as s1*s2 <= SIZE_MAX
 * if both s1 < MUL_NO_OVERFLOW and s2 < MUL_NO_OVERFLOW
 */
#define MUL_NO_OVERFLOW	((size_t)1 << (sizeof(size_t) * 4))

static inline bool umull_overflow(size_t a, size_t b, size_t *out)
{
    if ((a >= MUL_NO_OVERFLOW || b >= MUL_NO_OVERFLOW) && a > 0 && SIZE_MAX / a < b)
        return true;
    *out = a * b;
    return false;
}
#else
#define umull_overflow __builtin_mul_overflow
#endif

void *reallocarray(void *optr, size_t nmemb, size_t size)
{
    size_t total_size;
    if (UNLIKELY(umull_overflow(nmemb, size, &total_size))) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(optr, total_size);
}
#endif /* HAVE_REALLOCARRAY */

#if !defined(HAVE_READAHEAD)
ssize_t readahead(int fd __attribute__((unused)),
                  off_t offset __attribute__((unused)),
                  size_t count __attribute__((unused)))
{
    return 0;
}
#endif

#if !defined(HAVE_GET_CURRENT_DIR_NAME)
#include <limits.h>

char *get_current_dir_name(void)
{
    char buffer[PATH_MAX];
    char *ret;

    ret = getcwd(buffer, sizeof(buffer));
    return strdup(ret ? ret : "/");
}
#endif
