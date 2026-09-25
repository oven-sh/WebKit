// runstat: run a command, report wall time, max RSS, and CPU time per thread group.
// Per-thread user/sys CPU comes from /proc/<pid>/task/<tid>/stat, polled every 2 ms; the last sample of
// each thread is kept (a thread's stat file disappears when the thread exits).
// Output: one line of key=value pairs on stderr (or the file named by RUNSTAT_OUT).
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAXT 4096
struct T { int tid; char comm[32]; unsigned long long ut, st; };
static struct T threads[MAXT];
static int nthreads;

static struct T* find(int tid) {
    for (int i = 0; i < nthreads; i++) if (threads[i].tid == tid) return &threads[i];
    if (nthreads == MAXT) return NULL;
    struct T* t = &threads[nthreads++]; memset(t, 0, sizeof(*t)); t->tid = tid; return t;
}

static void sample(pid_t pid) {
    char path[128]; snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR* d = opendir(path); if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int tid = atoi(e->d_name);
        char sp[192]; snprintf(sp, sizeof sp, "/proc/%d/task/%d/stat", pid, tid);
        int fd = open(sp, O_RDONLY); if (fd < 0) continue;
        char buf[1024]; ssize_t n = read(fd, buf, sizeof buf - 1); close(fd);
        if (n <= 0) continue; buf[n] = 0;
        char* lp = strchr(buf, '('); char* rp = strrchr(buf, ')'); if (!lp || !rp) continue;
        struct T* t = find(tid); if (!t) continue;
        size_t cl = (size_t)(rp - lp - 1); if (cl > sizeof t->comm - 1) cl = sizeof t->comm - 1;
        memcpy(t->comm, lp + 1, cl); t->comm[cl] = 0;
        // after ") ": state(3) ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15)
        char* p = rp + 2; int field = 3; unsigned long long ut = 0, st = 0;
        while (*p && field <= 15) {
            char* end = p; while (*end && *end != ' ') end++;
            if (field == 14) ut = strtoull(p, NULL, 10);
            if (field == 15) st = strtoull(p, NULL, 10);
            field++; p = *end ? end + 1 : end;
        }
        if (ut >= t->ut) t->ut = ut;
        if (st >= t->st) t->st = st;
    }
    closedir(d);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: runstat cmd args...\n"); return 2; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (!pid) { execvp(argv[1], argv + 1); perror("execvp"); _exit(127); }
    int status = 0; struct rusage ru;
    for (;;) {
        pid_t r = wait4(pid, &status, WNOHANG, &ru);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) break;
        sample(pid);
        struct timespec ts = { 0, 2 * 1000 * 1000 }; nanosleep(&ts, NULL);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    long hz = sysconf(_SC_CLK_TCK);
    double mainU = 0, mainS = 0, jitU = 0, jitS = 0, gcU = 0, gcS = 0, otherU = 0, otherS = 0;
    for (int i = 0; i < nthreads; i++) {
        struct T* t = &threads[i]; double u = (double)t->ut / hz, s = (double)t->st / hz;
        if (t->tid == pid) { mainU += u; mainS += s; }
        else if (strstr(t->comm, "JIT") || strstr(t->comm, "Worklist") || strstr(t->comm, "DFG") || strstr(t->comm, "FTL") || strstr(t->comm, "Baseline")) { jitU += u; jitS += s; }
        else if (strstr(t->comm, "Heap") || strstr(t->comm, "GC") || strstr(t->comm, "Collector") || strstr(t->comm, "Marker")) { gcU += u; gcS += s; }
        else { otherU += u; otherS += s; }
    }
    const char* out = getenv("RUNSTAT_OUT");
    FILE* f = out ? fopen(out, "a") : stderr; if (!f) f = stderr;
    fprintf(f, "exit=%d wall=%.3f main_user=%.2f main_sys=%.2f jit_cpu=%.2f gc_cpu=%.2f other_cpu=%.2f total_user=%.2f total_sys=%.2f maxrss_kb=%ld threads=%d\n",
        WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), wall, mainU, mainS, jitU + jitS, gcU + gcS, otherU + otherS,
        ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6, ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, ru.ru_maxrss, nthreads);
    if (getenv("RUNSTAT_THREADS")) for (int i = 0; i < nthreads; i++) fprintf(f, "  tid=%d comm=%s user=%.2f sys=%.2f\n", threads[i].tid, threads[i].comm, (double)threads[i].ut / hz, (double)threads[i].st / hz);
    if (f != stderr) fclose(f);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
