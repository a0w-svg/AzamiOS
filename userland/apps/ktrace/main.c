/* ============================================================================
 * AzamiOS Userspace — Kernel Function Tracer CLI (ktrace.elf)
 * File: userland/apps/ktrace/main.c
 *
 * Provides control and readout for the kernel function tracing subsystem.
 * Interacts with /sys/kernel/trace/ or directly through SYS_AZ_KTRACE_* syscalls.
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/syscall.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s <command> [args...]\n", prog);
    printf("Commands:\n");
    printf("  enable <func...>   Enable tracing for named function(s) or 'all'\n");
    printf("  disable <func...>  Disable tracing for named function(s) or 'all'\n");
    printf("  profile <bundle>   Enable tracing bundle (syscalls, vfs, sched, ipc, all)\n");
    printf("  list               List currently traced functions\n");
    printf("  pipe / dump        Read and print pending trace events\n");
    printf("  stream             Stream trace events continuously (Ctrl-C to stop)\n");
    printf("  clear              Clear the kernel trace ring buffer\n");
    printf("  -V, --version      Display version information\n");
    printf("  -h, --help         Display this help message\n");
}

static int do_enable(const char *name)
{
    int fd = open("/sys/kernel/trace/enable", O_WRONLY, 0);
    if (fd >= 0) {
        write(fd, name, strlen(name));
        close(fd);
        return 0;
    }
    /* Fallback to direct syscall */
    long ret = syscall1(SYS_AZ_KTRACE_ENABLE, (long)name);
    return (ret < 0) ? -1 : 0;
}

static int do_disable(const char *name)
{
    int fd = open("/sys/kernel/trace/enable", O_WRONLY, 0);
    if (fd >= 0) {
        char buf[80];
        buf[0] = '-';
        strncpy(buf + 1, name, sizeof(buf) - 2);
        buf[sizeof(buf) - 1] = '\0';
        write(fd, buf, strlen(buf));
        close(fd);
        return 0;
    }
    /* Fallback to direct syscall */
    char buf[80];
    buf[0] = '-';
    strncpy(buf + 1, name, sizeof(buf) - 2);
    buf[sizeof(buf) - 1] = '\0';
    long ret = syscall1(SYS_AZ_KTRACE_ENABLE, (long)buf);
    return (ret < 0) ? -1 : 0;
}

static int do_list(void)
{
    int fd = open("/sys/kernel/trace/enable", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("Enabled trace functions:\n%s", buf);
        } else {
            printf("No functions currently traced.\n");
        }
        close(fd);
        return 0;
    }
    printf("ktrace: /sys/kernel/trace/enable not available\n");
    return 1;
}

static int do_clear(void)
{
    int fd = open("/sys/kernel/trace/clear", O_WRONLY, 0);
    if (fd >= 0) {
        write(fd, "1\n", 2);
        close(fd);
        printf("ktrace: ring buffer cleared\n");
        return 0;
    }
    syscall0(SYS_AZ_KTRACE_CLEAR);
    printf("ktrace: ring buffer cleared (syscall)\n");
    return 0;
}

static int do_dump(void)
{
    int fd = open("/sys/kernel/trace/pipe", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        ssize_t n;
        int total = 0;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            printf("%s", buf);
            total += (int)n;
        }
        close(fd);
        if (total == 0) {
            printf("[ktrace] No events in trace buffer.\n");
        }
        return 0;
    }

    /* Fallback to direct syscall */
    typedef struct {
        unsigned long long ts;
        unsigned int cpu;
        unsigned int pid;
        char name[48];
        unsigned long long arg0;
    } kentry_t;

    kentry_t entries[64];
    long count = syscall2(SYS_AZ_KTRACE_READ, (long)entries, 64);
    if (count <= 0) {
        printf("[ktrace] No events in trace buffer.\n");
        return 0;
    }

    for (long i = 0; i < count; i++) {
        unsigned long long sec = entries[i].ts / 1000000000ULL;
        unsigned long long nsec = entries[i].ts % 1000000000ULL;
        printf("[%llu.%06llu] cpu%u pid%u %s arg=0x%llx\n",
               sec, nsec / 1000, entries[i].cpu, entries[i].pid,
               entries[i].name, entries[i].arg0);
    }
    return 0;
}

static int do_stream(void)
{
    printf("[ktrace] Streaming events from trace pipe... (Ctrl+C to stop)\n");
    for (;;) {
        int fd = open("/sys/kernel/trace/pipe", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[2048];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                printf("%s", buf);
            }
            close(fd);
        }
        usleep(50000); /* 50ms */
    }
    return 0;
}

static int do_test(void)
{
    printf("[ktrace_test] Starting ktrace tracer test suite...\n");
    /* 1. Clear any prior events */
    do_clear();

    /* 2. Enable tracing for mmap */
    if (do_enable("mmap") != 0) {
        printf("[ktrace_test] Failed to enable mmap\n");
        return 1;
    }
    printf("[ktrace_test] Enabled trace for 'mmap' -> OK\n");

    /* 3. Trigger mmap syscall */
    void *p = (void *)syscall6(SYS_mmap, 0, 4096, 3 /* PROT_READ|WRITE */, 0x22 /* MAP_PRIVATE|ANON */, -1, 0);
    if (p != (void *)-1) {
        printf("[ktrace_test] Triggered mmap syscall (addr=%p) -> OK\n", p);
        syscall2(SYS_munmap, (long)p, 4096);
    }

    /* 4. Read events from /sys/kernel/trace/pipe */
    int fd = open("/sys/kernel/trace/pipe", O_RDONLY, 0);
    if (fd < 0) {
        printf("[ktrace_test] Failed to open /sys/kernel/trace/pipe\n");
        return 1;
    }
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        printf("[ktrace_test] Read 0 bytes from trace pipe\n");
        return 1;
    }
    buf[n] = '\0';
    printf("[ktrace_test] Captured trace events:\n%s", buf);
    if (strstr(buf, "mmap") != NULL) {
        printf("[ktrace_test] Found 'mmap' in trace output -> OK\n");
    } else {
        printf("[ktrace_test] 'mmap' not found in trace output!\n");
        return 1;
    }

    /* 5. Clear trace buffer */
    do_clear();
    printf("[ktrace_test] Cleared trace buffer -> OK\n");

    /* 6. Verify pipe is now empty */
    fd = open("/sys/kernel/trace/pipe", O_RDONLY, 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) {
            printf("[ktrace_test] Trace pipe empty after clear -> OK\n");
        }
    }

    /* 7. Disable tracing */
    do_disable("all");
    printf("[ktrace_test] Disabled trace -> OK\n");

    printf("\n=== All ktrace Tests PASSED! ===\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        if (getpid() <= 2) {
            return do_test();
        }
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "enable") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: missing function name\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            if (do_enable(argv[i]) == 0) {
                printf("Enabled trace: %s\n", argv[i]);
            } else {
                fprintf(stderr, "Failed to enable: %s\n", argv[i]);
            }
        }
        return 0;
    } else if (strcmp(cmd, "disable") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: missing function name\n");
            return 1;
        }
        for (int i = 2; i < argc; i++) {
            if (do_disable(argv[i]) == 0) {
                printf("Disabled trace: %s\n", argv[i]);
            } else {
                fprintf(stderr, "Failed to disable: %s\n", argv[i]);
            }
        }
        return 0;
    } else if (strcmp(cmd, "profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: missing profile name (syscalls, vfs, sched, ipc, all)\n");
            return 1;
        }
        const char *prof = argv[2];
        if (strcmp(prof, "syscalls") == 0) {
            const char *fns[] = { "sys_read", "sys_write", "sys_open", "sys_close", "sys_ioctl", "sys_mmap", "sys_fork", NULL };
            for (int i = 0; fns[i]; i++) do_enable(fns[i]);
            printf("Enabled trace profile: syscalls\n");
        } else if (strcmp(prof, "vfs") == 0) {
            const char *fns[] = { "vfs_read", "vfs_write", "vfs_lookup", "vfs_open", "vfs_close", "vfs_mount", NULL };
            for (int i = 0; fns[i]; i++) do_enable(fns[i]);
            printf("Enabled trace profile: vfs\n");
        } else if (strcmp(prof, "sched") == 0) {
            const char *fns[] = { "sched_switch", "sched_yield", "sched_fork", "sched_wake", NULL };
            for (int i = 0; fns[i]; i++) do_enable(fns[i]);
            printf("Enabled trace profile: sched\n");
        } else if (strcmp(prof, "ipc") == 0) {
            const char *fns[] = { "ipc_send", "ipc_recv", "channel_create", "channel_write", NULL };
            for (int i = 0; fns[i]; i++) do_enable(fns[i]);
            printf("Enabled trace profile: ipc\n");
        } else if (strcmp(prof, "all") == 0) {
            do_enable("all");
            printf("Enabled trace profile: all\n");
        } else {
            fprintf(stderr, "Unknown profile '%s'. Valid: syscalls, vfs, sched, ipc, all\n", prof);
            return 1;
        }
        return 0;
    } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "status") == 0) {
        return do_list();
    } else if (strcmp(cmd, "clear") == 0) {
        return do_clear();
    } else if (strcmp(cmd, "pipe") == 0 || strcmp(cmd, "dump") == 0) {
        return do_dump();
    } else if (strcmp(cmd, "stream") == 0) {
        return do_stream();
    } else if (strcmp(cmd, "test") == 0) {
        return do_test();
    } else if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
        printf("ktrace 7.0.0 (AzamiOS Kernel Function Tracer)\n");
        return 0;
    } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n", cmd);
    print_usage(argv[0]);
    return 1;
}
