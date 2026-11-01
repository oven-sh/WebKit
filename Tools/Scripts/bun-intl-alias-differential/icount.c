// Count user-space instructions of a child process (inherit to threads), like `perf stat -e instructions:u`.
#define _GNU_SOURCE
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    struct perf_event_attr pe; memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_HARDWARE; pe.size = sizeof(pe); pe.config = PERF_COUNT_HW_INSTRUCTIONS;
    pe.disabled = 1; pe.exclude_kernel = 1; pe.exclude_hv = 1; pe.inherit = 1; pe.enable_on_exec = 1;
    int fd = (int)syscall(SYS_perf_event_open, &pe, 0, -1, -1, 0);
    if (fd < 0) { perror("perf_event_open"); return 1; }
    pid_t pid = fork();
    if (!pid) { execvp(argv[1], argv + 1); _exit(127); }
    int st; waitpid(pid, &st, 0);
    uint64_t c = 0; if (read(fd, &c, sizeof(c)) != sizeof(c)) return 1;
    fprintf(stderr, "instructions:u %llu\n", (unsigned long long)c);
    return WEXITSTATUS(st);
}
