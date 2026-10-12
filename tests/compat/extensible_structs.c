/* SPDX-License-Identifier: MIT */
/* ============================================================================
 * AzamiOS — Extensible-Struct Syscall Protocol Verification Suite
 * File: tests/compat/extensible_structs.c
 *
 * Implements Workstream B3.3: verifies copy_struct_from_user semantics across
 * extensible syscalls:
 *   - openat2
 *   - clone3
 *   - sched_setattr
 *   - sched_getattr
 *   - perf_event_open
 *
 * Checks:
 *   - size = min - 1 -> expected reject (EINVAL or E2BIG per Linux oracle)
 *   - size = min -> success / valid behavior
 *   - size = min/ksize + 8 (zero tail) -> accepted
 *   - size = min/ksize + 8 (non-zero tail) -> E2BIG
 *   - size > 4096 -> E2BIG
 *   - invalid pointer -> EFAULT
 * ============================================================================ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifndef SYS_openat2
# define SYS_openat2 437
#endif
#ifndef SYS_clone3
# define SYS_clone3 435
#endif
#ifndef SYS_sched_setattr
# define SYS_sched_setattr 314
#endif
#ifndef SYS_sched_getattr
# define SYS_sched_getattr 315
#endif
#ifndef SYS_perf_event_open
# define SYS_perf_event_open 298
#endif

#define OPEN_HOW_SIZE_VER0 24
struct compat_open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

#define CLONE_ARGS_SIZE_VER0 64
struct compat_clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

#define SCHED_ATTR_SIZE_VER0 48
#define SCHED_ATTR_SIZE_VER1 56
struct compat_sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
    uint32_t sched_util_min;
    uint32_t sched_util_max;
};

#define PERF_ATTR_SIZE_VER0 64
#define PERF_ATTR_SIZE_VER5 112
struct compat_perf_event_attr {
    uint32_t type;
    uint32_t size;
    uint64_t config;
    union {
        uint64_t sample_period;
        uint64_t sample_freq;
    };
    uint64_t sample_type;
    uint64_t read_format;
    uint64_t flags;
    uint32_t wakeup_events;
    uint32_t bp_type;
    uint64_t bp_addr;
    uint64_t bp_len;
    uint64_t branch_sample_type;
    uint64_t sample_regs_user;
    uint32_t sample_stack_user;
    int32_t  clockid;
    uint64_t sample_regs_intr;
    uint32_t aux_watermark;
    uint16_t sample_max_stack;
    uint16_t __reserved_2;
};

static int g_tests_run = 0;
static int g_tests_failed = 0;

static void check_result(const char *name, long ret, int err, int expected_errno, int expect_success)
{
    g_tests_run++;
    if (expect_success) {
        if (ret >= 0) {
            printf("  ✓ %s: SUCCESS (ret=%ld)\n", name, ret);
        } else {
            printf("  ✗ %s: FAILED (expected success, got ret=%ld, errno=%d [%s])\n",
                   name, ret, err, strerror(err));
            g_tests_failed++;
        }
    } else {
        if (ret < 0 && err == expected_errno) {
            printf("  ✓ %s: SUCCESS (rejected with errno %d [%s])\n",
                   name, err, strerror(err));
        } else {
            printf("  ✗ %s: FAILED (expected errno %d [%s], got ret=%ld, errno=%d [%s])\n",
                   name, expected_errno, strerror(expected_errno), ret, err, strerror(err));
            g_tests_failed++;
        }
    }
}

static void test_openat2(void)
{
    printf("[EXT_STRUCT] Testing openat2 extensible-struct protocol...\n");

    /* 1. size = min - 1 (23) -> EINVAL */
    struct { struct compat_open_how h; uint64_t extra; } ext;
    memset(&ext, 0, sizeof(ext));
    errno = 0;
    long r = syscall(SYS_openat2, AT_FDCWD, "/dev/null", &ext, 23);
    check_result("openat2 size=min-1", r, errno, EINVAL, 0);

    /* 2. size = min (24) -> Success */
    errno = 0;
    r = syscall(SYS_openat2, AT_FDCWD, "/dev/null", &ext, 24);
    if (r >= 0) close((int)r);
    check_result("openat2 size=min", r, errno, 0, 1);

    /* 3. size = min + 8 (32), zero tail -> Success */
    ext.extra = 0;
    errno = 0;
    r = syscall(SYS_openat2, AT_FDCWD, "/dev/null", &ext, 32);
    if (r >= 0) close((int)r);
    check_result("openat2 size=min+8 (zero tail)", r, errno, 0, 1);

    /* 4. size = min + 8 (32), non-zero tail -> E2BIG */
    ext.extra = 1;
    errno = 0;
    r = syscall(SYS_openat2, AT_FDCWD, "/dev/null", &ext, 32);
    if (r >= 0) close((int)r);
    check_result("openat2 size=min+8 (nonzero tail)", r, errno, E2BIG, 0);

    /* 5. size > 4096 (4097) -> E2BIG */
    ext.extra = 0;
    errno = 0;
    r = syscall(SYS_openat2, AT_FDCWD, "/dev/null", &ext, 4097);
    if (r >= 0) close((int)r);
    check_result("openat2 size>4096", r, errno, E2BIG, 0);

    /* 6. invalid pointer -> EFAULT */
    errno = 0;
    r = syscall(SYS_openat2, AT_FDCWD, "/dev/null", (void *)1, 24);
    if (r >= 0) close((int)r);
    check_result("openat2 invalid pointer", r, errno, EFAULT, 0);
}

static void test_clone3(void)
{
    printf("[EXT_STRUCT] Testing clone3 extensible-struct protocol...\n");

    /* 1. size = min - 1 (63) -> EINVAL */
    struct { struct compat_clone_args c; uint64_t extra; } cext;
    memset(&cext, 0, sizeof(cext));
    cext.c.exit_signal = SIGCHLD;
    errno = 0;
    long r = syscall(SYS_clone3, &cext, 63);
    if (r == 0) _exit(0);
    /* In container or environment where clone3 syscall may be blocked/ENOSYS by seccomp on host */
    if (r < 0 && (errno == ENOSYS || errno == EPERM)) {
        printf("  - clone3 unavailable in host container (errno %d), skipping...\n", errno);
        return;
    }
    check_result("clone3 size=min-1", r, errno, EINVAL, 0);

    /* 2. size = min (64, CLONE_ARGS_SIZE_VER0) -> Success */
    memset(&cext, 0, sizeof(cext));
    cext.c.exit_signal = SIGCHLD;
    errno = 0;
    r = syscall(SYS_clone3, &cext, 64);
    if (r == 0) _exit(0);
    if (r > 0) {
        int st;
        waitpid((pid_t)r, &st, 0);
    }
    check_result("clone3 size=min(64)", r, errno, 0, 1);

    /* 3. size = ksize (88, CLONE_ARGS_SIZE_VER2) -> Success */
    memset(&cext, 0, sizeof(cext));
    cext.c.exit_signal = SIGCHLD;
    errno = 0;
    r = syscall(SYS_clone3, &cext, sizeof(struct compat_clone_args));
    if (r == 0) _exit(0);
    if (r > 0) {
        int st;
        waitpid((pid_t)r, &st, 0);
    }
    check_result("clone3 size=ksize(88)", r, errno, 0, 1);

    /* 4. size = ksize + 8 (96), zero tail -> Success */
    memset(&cext, 0, sizeof(cext));
    cext.c.exit_signal = SIGCHLD;
    cext.extra = 0;
    errno = 0;
    r = syscall(SYS_clone3, &cext, sizeof(cext));
    if (r == 0) _exit(0);
    if (r > 0) {
        int st;
        waitpid((pid_t)r, &st, 0);
    }
    check_result("clone3 size=ksize+8 (zero tail)", r, errno, 0, 1);

    /* 5. size = ksize + 8 (96), non-zero tail -> E2BIG */
    memset(&cext, 0, sizeof(cext));
    cext.c.exit_signal = SIGCHLD;
    cext.extra = 0x1234;
    errno = 0;
    r = syscall(SYS_clone3, &cext, sizeof(cext));
    if (r == 0) _exit(0);
    check_result("clone3 size=ksize+8 (nonzero tail)", r, errno, E2BIG, 0);

    /* 6. size > 4096 -> E2BIG */
    cext.extra = 0;
    errno = 0;
    r = syscall(SYS_clone3, &cext, 4097);
    if (r == 0) _exit(0);
    check_result("clone3 size>4096", r, errno, E2BIG, 0);

    /* 7. invalid pointer -> EFAULT */
    errno = 0;
    r = syscall(SYS_clone3, (void *)1, 64);
    if (r == 0) _exit(0);
    check_result("clone3 invalid pointer", r, errno, EFAULT, 0);
}

static void test_sched_setattr(void)
{
    printf("[EXT_STRUCT] Testing sched_setattr extensible-struct protocol...\n");

    /* 1. size = min - 1 (47) -> E2BIG (Linux oracle) */
    struct { struct compat_sched_attr a; uint64_t extra; } sext;
    memset(&sext, 0, sizeof(sext));
    sext.a.size = 47;
    errno = 0;
    long r = syscall(SYS_sched_setattr, 0, &sext, 0);
    check_result("sched_setattr size=min-1", r, errno, E2BIG, 0);

    /* 2. size = min (48, SCHED_ATTR_SIZE_VER0) -> Success */
    memset(&sext, 0, sizeof(sext));
    sext.a.size = 48;
    sext.a.sched_policy = 0; /* SCHED_OTHER */
    errno = 0;
    r = syscall(SYS_sched_setattr, 0, &sext, 0);
    check_result("sched_setattr size=min(48)", r, errno, 0, 1);

    /* 3. size = ksize (56, SCHED_ATTR_SIZE_VER1) -> Success */
    memset(&sext, 0, sizeof(sext));
    sext.a.size = 56;
    sext.a.sched_policy = 0;
    errno = 0;
    r = syscall(SYS_sched_setattr, 0, &sext, 0);
    check_result("sched_setattr size=ksize(56)", r, errno, 0, 1);

    /* 4. size = ksize + 8 (64), zero tail -> Success */
    memset(&sext, 0, sizeof(sext));
    sext.a.size = sizeof(sext);
    sext.a.sched_policy = 0;
    sext.extra = 0;
    errno = 0;
    r = syscall(SYS_sched_setattr, 0, &sext, 0);
    check_result("sched_setattr size=ksize+8 (zero tail)", r, errno, 0, 1);

    /* 5. size = ksize + 8 (64), non-zero tail -> E2BIG */
    sext.extra = 1;
    errno = 0;
    r = syscall(SYS_sched_setattr, 0, &sext, 0);
    check_result("sched_setattr size=ksize+8 (nonzero tail)", r, errno, E2BIG, 0);

    /* 6. size > 4096 -> E2BIG */
    sext.a.size = 4097;
    sext.extra = 0;
    errno = 0;
    r = syscall(SYS_sched_setattr, 0, &sext, 0);
    check_result("sched_setattr size>4096", r, errno, E2BIG, 0);

    /* 7. invalid pointer -> EFAULT */
    errno = 0;
    r = syscall(SYS_sched_setattr, 0, (void *)1, 0);
    check_result("sched_setattr invalid pointer", r, errno, EFAULT, 0);
}

static void test_sched_getattr(void)
{
    printf("[EXT_STRUCT] Testing sched_getattr extensible-struct protocol...\n");

    /* 1. size = min - 1 (47) -> EINVAL (Linux oracle) */
    char buf[128];
    memset(buf, 0xaa, sizeof(buf));
    errno = 0;
    long r = syscall(SYS_sched_getattr, 0, buf, 47, 0);
    check_result("sched_getattr size=min-1", r, errno, EINVAL, 0);

    /* 2. size = min (48, SCHED_ATTR_SIZE_VER0) -> Success */
    memset(buf, 0xaa, sizeof(buf));
    errno = 0;
    r = syscall(SYS_sched_getattr, 0, buf, 48, 0);
    check_result("sched_getattr size=min(48)", r, errno, 0, 1);

    /* 3. size = ksize (56) -> Success */
    memset(buf, 0xaa, sizeof(buf));
    errno = 0;
    r = syscall(SYS_sched_getattr, 0, buf, 56, 0);
    check_result("sched_getattr size=ksize(56)", r, errno, 0, 1);

    /* 4. size = 64 (ksize + 8), verify zero tail written to user */
    memset(buf, 0xaa, sizeof(buf));
    errno = 0;
    r = syscall(SYS_sched_getattr, 0, buf, 64, 0);
    int zero_tail = 1;
    for (int i = 56; i < 64; i++) {
        if (buf[i] != 0) { zero_tail = 0; break; }
    }
    check_result("sched_getattr size=ksize+8", r, errno, 0, zero_tail && (r >= 0));

    /* 5. size > 4096 -> EINVAL (Linux oracle) */
    errno = 0;
    r = syscall(SYS_sched_getattr, 0, buf, 4097, 0);
    check_result("sched_getattr size>4096", r, errno, EINVAL, 0);

    /* 6. invalid pointer -> EFAULT */
    errno = 0;
    r = syscall(SYS_sched_getattr, 0, (void *)1, 48, 0);
    check_result("sched_getattr invalid pointer", r, errno, EFAULT, 0);
}

static void test_perf_event_open(void)
{
    printf("[EXT_STRUCT] Testing perf_event_open extensible-struct protocol...\n");

    /* Check if perf_event_open is permitted in environment */
    struct { struct compat_perf_event_attr a; uint64_t extra; } pext;
    memset(&pext, 0, sizeof(pext));
    pext.a.type = 1; /* PERF_TYPE_SOFTWARE */
    pext.a.size = PERF_ATTR_SIZE_VER5;
    pext.a.config = 0; /* PERF_COUNT_SW_CPU_CLOCK */
    errno = 0;
    long probe_r = syscall(SYS_perf_event_open, &pext, 0, -1, -1, 0);
    if (probe_r < 0 && (errno == EACCES || errno == EPERM)) {
        printf("  - perf_event_open restricted by host permissions (errno %d), skipping...\n", errno);
        return;
    }
    if (probe_r >= 0) close((int)probe_r);

    /* 1. size = min - 1 (63) -> E2BIG */
    pext.a.size = 63;
    errno = 0;
    long r = syscall(SYS_perf_event_open, &pext, 0, -1, -1, 0);
    check_result("perf_event_open size=min-1", r, errno, E2BIG, 0);

    /* 2. size = 112 (PERF_ATTR_SIZE_VER5) -> Success or valid fd */
    pext.a.size = PERF_ATTR_SIZE_VER5;
    errno = 0;
    r = syscall(SYS_perf_event_open, &pext, 0, -1, -1, 0);
    if (r >= 0) close((int)r);
    check_result("perf_event_open size=ksize", r, errno, 0, 1);

    /* 3. size = 120 (ksize + 8), zero tail -> Success or valid fd */
    memset(&pext, 0, sizeof(pext));
    pext.a.type = 1;
    pext.a.size = 120;
    pext.a.config = 0;
    pext.extra = 0;
    errno = 0;
    r = syscall(SYS_perf_event_open, &pext, 0, -1, -1, 0);
    if (r >= 0) close((int)r);
    check_result("perf_event_open size=ksize+8 (zero tail)", r, errno, 0, 1);

    /* 4. size = 120, non-zero tail -> E2BIG */
    pext.extra = 1;
    errno = 0;
    r = syscall(SYS_perf_event_open, &pext, 0, -1, -1, 0);
    if (r >= 0) close((int)r);
    check_result("perf_event_open size=ksize+8 (nonzero tail)", r, errno, E2BIG, 0);

    /* 5. size > 4096 -> E2BIG */
    pext.a.size = 4097;
    pext.extra = 0;
    errno = 0;
    r = syscall(SYS_perf_event_open, &pext, 0, -1, -1, 0);
    if (r >= 0) close((int)r);
    check_result("perf_event_open size>4096", r, errno, E2BIG, 0);

    /* 6. invalid pointer -> EFAULT */
    errno = 0;
    r = syscall(SYS_perf_event_open, (void *)1, 0, -1, -1, 0);
    if (r >= 0) close((int)r);
    check_result("perf_event_open invalid pointer", r, errno, EFAULT, 0);
}

int main(void)
{
    printf("== AzamiOS Extensible-Struct Verification (Workstream B3.3) ==\n");
    test_openat2();
    test_clone3();
    test_sched_setattr();
    test_sched_getattr();
    test_perf_event_open();

    printf("== Summary: %d tests, %d failed ==\n", g_tests_run, g_tests_failed);
    if (g_tests_failed == 0) {
        printf("PASS: All extensible-struct test cases verified!\n");
        return 0;
    }
    return 1;
}
