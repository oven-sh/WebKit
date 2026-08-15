// This code is copied from libuv. Thanks to libuv developers.
/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "config.h"
#include <wtf/Platform.h>
#include <cstdint>

#if OS(LINUX)

#ifndef PRIu64
#define PRIu64 "lu"
#endif

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <climits>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdlib>
#include <cstdio>

#ifndef assert
static inline void assert(bool)
{
}
#endif

static int uv__open_cloexec(const char* path, int flags)
{
    int fd = -1;
    do {
        fd = open(path, flags | O_CLOEXEC, 0);
    } while (fd == -1 && errno == EINTR);
    
    if (fd == -1)
        return -1;

    return fd;
}

static int uv__close_nocheckstdio(int fd)
{
    int saved_errno;
    int rc;

    saved_errno = errno;
    rc = close(fd);
    if (rc == -1) {
        if (errno == EINTR || errno == EINPROGRESS)
            rc = 0; /* The close is in progress, not an error. */
        errno = saved_errno;
    }

    return rc;
}

static int uv__slurp(const char* filename, char* buf, size_t len)
{
    ssize_t n;
    int fd;

    assert(len > 0);

    fd = uv__open_cloexec(filename, O_RDONLY);
    if (fd < 0)
        return fd;

    do
        n = read(fd, buf, len - 1);
    while (n == -1 && errno == EINTR);

    if (uv__close_nocheckstdio(fd))
        abort();

    if (n < 0)
        return errno;

    buf[n] = '\0';

    return 0;
}

static uint64_t uv__read_uint64(const char* filename)
{
    char buf[32]; /* Large enough to hold an encoded uint64_t. */
    uint64_t rc;

    rc = 0;
    if (0 == uv__slurp(filename, buf, sizeof(buf)))
        if (1 != sscanf(buf, "%" PRIu64, &rc))
            if (0 == strcmp(buf, "max\n"))
                rc = UINT64_MAX;

    return rc;
}

/* Given a buffer with the contents of a cgroup1 /proc/self/cgroups,
 * finds the location and length of the memory controller mount path.
 * This disregards the leading / for easy concatenation of paths.
 * Returns NULL if the memory controller wasn't found. */
static char* uv__cgroup1_find_memory_controller(char buf[1024],
    int* n)
{
    char* p;

    /* Seek to the memory controller line. */
    p = strchr(buf, ':');
    while (p != NULL && strncmp(p, ":memory:", 8)) {
        p = strchr(p, '\n');
        if (p != NULL)
            p = strchr(p, ':');
    }

    if (p != NULL) {
        /* Determine the length of the mount path. */
        p = p + strlen(":memory:/");
        *n = (int)strcspn(p, "\n");
    }

    return p;
}

/* Given a buffer with the contents of /proc/self/cgroup, finds the location
 * and length of the process's path in the cgroup v2 hierarchy, which is the
 * "0::<path>" line. This disregards the leading / for easy concatenation of
 * paths. Returns NULL if there is no such line (a pure cgroup v1 host).
 *
 * On a cgroup v2 host this line is usually the only one, but it is not
 * guaranteed to be: a cgroup v1 hierarchy mounted with only a name and no
 * controllers ("mount -t cgroup -o none,name=foo") coexists with cgroup v2 and
 * adds an "N:name=foo:<path>" line, and the kernel lists hierarchies by
 * descending id, so such lines sort before the "0::" line. */
static char* uv__cgroup2_find_path(char buf[1024], int* n)
{
    char* line = buf;

    while (*line != '\0') {
        if (strncmp(line, "0::", 3) == 0) {
            char* p = line + strlen("0::");
            if (*p == '/')
                p++;
            *n = (int)strcspn(p, "\n");
            return p;
        }

        line = strchr(line, '\n');
        if (line == NULL)
            break;
        line++;
    }

    return NULL;
}

static void uv__get_cgroup1_memory_limits(char buf[1024], uint64_t* high,
    uint64_t* max)
{
    char filename[4097];
    char* p;
    int n;
    uint64_t cgroup1_max;

    /* Find out where the controller is mounted. */
    p = uv__cgroup1_find_memory_controller(buf, &n);
    if (p != NULL) {
        snprintf(filename, sizeof(filename),
            "/sys/fs/cgroup/memory/%.*s/memory.soft_limit_in_bytes", n, p);
        *high = uv__read_uint64(filename);

        snprintf(filename, sizeof(filename),
            "/sys/fs/cgroup/memory/%.*s/memory.limit_in_bytes", n, p);
        *max = uv__read_uint64(filename);

        /* If the controller wasn't mounted, the reads above will have failed,
         * as indicated by uv__read_uint64 returning 0.
         */
        if (*high != 0 && *max != 0)
            goto update_limits;
    }

    /* Fall back to the limits of the global memory controller. */
    *high = uv__read_uint64("/sys/fs/cgroup/memory/memory.soft_limit_in_bytes");
    *max = uv__read_uint64("/sys/fs/cgroup/memory/memory.limit_in_bytes");

    /* uv__read_uint64 detects cgroup2's "max", so we need to separately detect
     * cgroup1's maximum value (which is derived from LONG_MAX and PAGE_SIZE).
     */
update_limits:
    cgroup1_max = LONG_MAX & ~(sysconf(_SC_PAGESIZE) - 1);
    if (*high == cgroup1_max)
        *high = UINT64_MAX;
    if (*max == cgroup1_max)
        *max = UINT64_MAX;
}

/* p and n locate the process's cgroup v2 path, see uv__cgroup2_find_path(). */
static void uv__get_cgroup2_memory_limits(const char* p, int n, uint64_t* high,
    uint64_t* max)
{
    char path[4097];
    char filename[4097];

    snprintf(path, sizeof(path), "/sys/fs/cgroup/%.*s", n, p);

    *high = UINT64_MAX;
    *max = UINT64_MAX;

    /* cgroup v2 limits are hierarchical: walk from the leaf to the root,
     * taking the tightest limit observed at any level. */
    while (strncmp(path, "/sys/fs/cgroup", sizeof("/sys/fs/cgroup") - 1) == 0) {
        uint64_t v;

        snprintf(filename, sizeof(filename), "%s/memory.max", path);
        v = uv__read_uint64(filename);
        if (v > 0 && v < *max)
            *max = v;

        snprintf(filename, sizeof(filename), "%s/memory.high", path);
        v = uv__read_uint64(filename);
        if (v > 0 && v < *high)
            *high = v;

        if (strcmp(path, "/sys/fs/cgroup") == 0)
            break;
        char* last_slash = strrchr(path, '/');
        if (last_slash == NULL)
            break;
        *last_slash = '\0';
    }
}

static uint64_t uv__get_cgroup_constrained_memory(char buf[1024])
{
    uint64_t high;
    uint64_t max;
    char* p;
    int n;

    /* A controller is bound to exactly one hierarchy. If the process has a
     * cgroup v1 memory controller line (cgroup v1 or hybrid host), the limits
     * live there; otherwise they live in the cgroup v2 hierarchy, if there is
     * one. The "0::" line is not necessarily the first line of the file, see
     * uv__cgroup2_find_path(). */
    if (uv__cgroup1_find_memory_controller(buf, &n) == NULL
        && (p = uv__cgroup2_find_path(buf, &n)) != NULL)
        uv__get_cgroup2_memory_limits(p, n, &high, &max);
    else
        uv__get_cgroup1_memory_limits(buf, &high, &max);

    if (high == 0 || max == 0)
        return 0;

    uint64_t result = high < max ? high : max;

#if CPU(X86_64) || CPU(ARM64)
    // max 52 bit integer, which is the most addressible space between arm64 and x64
    if (result >= 9007199254740991)
        return 0;
#endif

    return result;
}

// TODO: should we cache this? can we?
uint64_t uv_get_constrained_memory()
{
    char buf[1024];

    if (uv__slurp("/proc/self/cgroup", buf, sizeof(buf)))
        return 0;

    return uv__get_cgroup_constrained_memory(buf);
}

/* Find the cgroup v1 line whose controller list contains "cpu" as a whole
 * token (it may appear as "cpu", "cpu,cpuacct", or "cpuacct,cpu"). Returns
 * a pointer to the path component (after the leading "/") and its length. */
static char* uv__cgroup1_find_cpu_controller(char buf[1024], int* n)
{
    char* line = buf;
    while (line != NULL && *line != '\0') {
        char* ctl = strchr(line, ':');
        if (ctl == NULL) return NULL;
        ctl++;
        char* path = strchr(ctl, ':');
        if (path == NULL) return NULL;

        char* tok = ctl;
        while (tok < path) {
            int len = (int)strcspn(tok, ",:");
            if (len == 3 && strncmp(tok, "cpu", 3) == 0) {
                path++; /* skip ":" */
                if (*path == '/') path++;
                *n = (int)strcspn(path, "\n");
                return path;
            }
            tok += len + 1;
        }

        line = strchr(path, '\n');
        if (line != NULL) line++;
    }
    return NULL;
}

static int uv__get_cgroup1_constrained_cpu(char buf[1024])
{
    char filename[4097];
    char quota_buf[32];
    long long quota;
    long long period;
    int n = 0;

    char* p = uv__cgroup1_find_cpu_controller(buf, &n);
    if (p == NULL) {
        p = (char*)"";
        n = 0;
    }

    snprintf(filename, sizeof(filename),
        "/sys/fs/cgroup/cpu/%.*s/cpu.cfs_quota_us", n, p);
    if (uv__slurp(filename, quota_buf, sizeof(quota_buf)) != 0)
        return 0;
    if (sscanf(quota_buf, "%lld", &quota) != 1 || quota <= 0)
        return 0;

    snprintf(filename, sizeof(filename),
        "/sys/fs/cgroup/cpu/%.*s/cpu.cfs_period_us", n, p);
    if (uv__slurp(filename, quota_buf, sizeof(quota_buf)) != 0)
        return 0;
    if (sscanf(quota_buf, "%lld", &period) != 1 || period <= 0)
        return 0;

    int cpus = (int)(quota / period);
    return cpus < 1 ? 1 : cpus;
}

/* p and n locate the process's cgroup v2 path, see uv__cgroup2_find_path(). */
static int uv__get_cgroup2_constrained_cpu(const char* p, int n)
{
    char path[4097];
    char filename[4097];
    char quota_buf[64];
    long long quota;
    long long period;
    int min_cpus = 0;

    snprintf(path, sizeof(path), "/sys/fs/cgroup/%.*s", n, p);

    while (strncmp(path, "/sys/fs/cgroup", sizeof("/sys/fs/cgroup") - 1) == 0) {
        snprintf(filename, sizeof(filename), "%s/cpu.max", path);
        if (uv__slurp(filename, quota_buf, sizeof(quota_buf)) == 0
            && strncmp(quota_buf, "max", 3) != 0
            && sscanf(quota_buf, "%lld %lld", &quota, &period) == 2
            && period > 0 && quota > 0) {
            int cpus = (int)(quota / period);
            if (cpus < 1)
                cpus = 1;
            if (min_cpus == 0 || cpus < min_cpus)
                min_cpus = cpus;
        }

        if (strcmp(path, "/sys/fs/cgroup") == 0)
            break;
        char* last_slash = strrchr(path, '/');
        if (last_slash == NULL)
            break;
        *last_slash = '\0';
    }

    return min_cpus;
}

int uv_get_constrained_cpu()
{
    char buf[1024];
    char* p;
    int n;

    if (uv__slurp("/proc/self/cgroup", buf, sizeof(buf)))
        return 0;

    /* Same hierarchy selection as uv__get_cgroup_constrained_memory(), for
     * the cpu controller. */
    if (uv__cgroup1_find_cpu_controller(buf, &n) == NULL
        && (p = uv__cgroup2_find_path(buf, &n)) != NULL)
        return uv__get_cgroup2_constrained_cpu(p, n);
    return uv__get_cgroup1_constrained_cpu(buf);
}

#else

uint64_t uv_get_constrained_memory()
{
    return 0;
}

int uv_get_constrained_cpu()
{
    return 0;
}

#endif