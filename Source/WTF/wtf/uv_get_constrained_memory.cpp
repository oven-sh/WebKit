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

static void uv__get_cgroup2_memory_limits(char buf[1024], uint64_t* high,
    uint64_t* max)
{
    char path[4097];
    char filename[4097];
    char* p;
    int n;

    /* Find out where the controller is mounted. */
    p = buf + strlen("0::/");
    n = (int)strcspn(p, "\n");

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

    /* In the case of cgroupv2, we'll only have a single entry. */
    if (strncmp(buf, "0::/", 4))
        uv__get_cgroup1_memory_limits(buf, &high, &max);
    else
        uv__get_cgroup2_memory_limits(buf, &high, &max);

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

static uint64_t uv__get_cgroup1_current_memory(char buf[1024])
{
    char filename[4097];
    char* p;
    int n;

    p = uv__cgroup1_find_memory_controller(buf, &n);
    if (p != NULL) {
        snprintf(filename, sizeof(filename),
            "/sys/fs/cgroup/memory/%.*s/memory.usage_in_bytes", n, p);
        uint64_t current = uv__read_uint64(filename);
        if (current != 0)
            return current;
    }

    return uv__read_uint64("/sys/fs/cgroup/memory/memory.usage_in_bytes");
}

static uint64_t uv__get_cgroup2_current_memory(char buf[1024])
{
    char filename[4097];
    char* p;
    int n;

    p = buf + strlen("0::/");
    n = (int)strcspn(p, "\n");

    snprintf(filename, sizeof(filename),
        "/sys/fs/cgroup/%.*s/memory.current", n, p);
    return uv__read_uint64(filename);
}

uint64_t uv_get_cgroup_current_memory()
{
    char buf[1024];

    if (uv__slurp("/proc/self/cgroup", buf, sizeof(buf)))
        return 0;

    if (strncmp(buf, "0::/", 4))
        return uv__get_cgroup1_current_memory(buf);

    return uv__get_cgroup2_current_memory(buf);
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

    return (int)((quota + period - 1) / period);
}

static int uv__get_cgroup2_constrained_cpu(char buf[1024])
{
    char path[4097];
    char filename[4097];
    char quota_buf[64];
    long long quota;
    long long period;
    int min_cpus = 0;

    char* p = buf + strlen("0::/");
    int n = (int)strcspn(p, "\n");

    snprintf(path, sizeof(path), "/sys/fs/cgroup/%.*s", n, p);

    while (strncmp(path, "/sys/fs/cgroup", sizeof("/sys/fs/cgroup") - 1) == 0) {
        snprintf(filename, sizeof(filename), "%s/cpu.max", path);
        if (uv__slurp(filename, quota_buf, sizeof(quota_buf)) == 0
            && strncmp(quota_buf, "max", 3) != 0
            && sscanf(quota_buf, "%lld %lld", &quota, &period) == 2
            && period > 0 && quota > 0) {
            int cpus = (int)((quota + period - 1) / period);
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

    if (uv__slurp("/proc/self/cgroup", buf, sizeof(buf)))
        return 0;

    if (strncmp(buf, "0::/", 4) == 0)
        return uv__get_cgroup2_constrained_cpu(buf);
    return uv__get_cgroup1_constrained_cpu(buf);
}

#else

uint64_t uv_get_constrained_memory()
{
    return 0;
}

uint64_t uv_get_cgroup_current_memory()
{
    return 0;
}

int uv_get_constrained_cpu()
{
    return 0;
}

#endif