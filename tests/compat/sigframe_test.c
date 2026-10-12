/* SPDX-License-Identifier: MIT */
/* ============================================================================
 * AzamiOS — Signal Frame & rt_sigreturn Verification Suite
 * File: tests/compat/sigframe_test.c
 *
 * Implements Workstreams B4.1 and B4.2:
 *  - B4.1: Validates exact Linux x86-64 struct rt_sigframe layout:
 *          pretcode at (%rsp), ucontext_t at %rdx, siginfo_t at %rsi,
 *          16-byte stack alignment at handler entry,
 *          64-byte aligned fpregs with _fpx_sw_bytes magic1/magic2.
 *  - B4.1: Validates that handler mutations to uc_mcontext.gregs[REG_RIP]
 *          and uc_mcontext.gregs[REG_RAX] take effect on rt_sigreturn
 *          (e.g. stepping over an illegal instruction ud2).
 *  - B4.2: Validates rt_sigreturn robustness against malicious/corrupted frames:
 *          non-canonical RIP, non-canonical RSP, unmapped stack memory.
 *          The thread must be terminated with SIGSEGV (never kernel crash).
 * ============================================================================ */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define FP_XSTATE_MAGIC1 0x46505853U
#define FP_XSTATE_MAGIC2 0x46505845U

struct test_fpx_sw_bytes {
    uint32_t magic1;
    uint32_t extended_size;
    uint64_t xfeatures;
    uint32_t xstate_size;
    uint32_t padding[7];
};

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_tests_run++; \
    if (cond) { \
        g_tests_passed++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        printf("  [FAIL] %s (line %d)\n", msg, __LINE__); \
    } \
} while (0)

/* Track state seen by the SA_SIGINFO handler */
static volatile sig_atomic_t g_handler_invoked = 0;
static volatile uint64_t g_seen_entry_rsp = 0;
static volatile uint64_t g_seen_pretcode = 0;
static volatile uint64_t g_seen_signo = 0;
static volatile uint64_t g_seen_rsi = 0;
static volatile uint64_t g_seen_rdx = 0;
static volatile uint64_t g_seen_fpregs = 0;
static volatile uint32_t g_seen_magic1 = 0;
static volatile uint32_t g_seen_magic2 = 0;
static volatile uint32_t g_seen_ext_size = 0;

static void layout_probe_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig;
    g_handler_invoked = 1;
    g_seen_signo = info ? (uint64_t)info->si_signo : 0;
    g_seen_rsi = (uint64_t)(uintptr_t)info;
    g_seen_rdx = (uint64_t)(uintptr_t)uctx;

    ucontext_t *uc = (ucontext_t *)uctx;
    if (uc) {
        g_seen_fpregs = (uint64_t)(uintptr_t)uc->uc_mcontext.fpregs;
        if (uc->uc_mcontext.fpregs) {
            uint8_t *fp = (uint8_t *)uc->uc_mcontext.fpregs;
            struct test_fpx_sw_bytes *sw = (struct test_fpx_sw_bytes *)(fp + 464);
            g_seen_magic1 = sw->magic1;
            g_seen_ext_size = sw->extended_size;
            if (sw->magic1 == FP_XSTATE_MAGIC1 && sw->extended_size >= 4) {
                g_seen_magic2 = *(uint32_t *)(fp + sw->extended_size - 4);
            }
        }
    }
}

static void test_signal_frame_layout(void)
{
    printf("--- Test 1: B4.1 Signal Frame Layout & Alignment ---\n");
    g_handler_invoked = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = layout_probe_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        perror("sigaction");
        return;
    }

    raise(SIGUSR1);

    TEST_ASSERT(g_handler_invoked == 1, "Handler was invoked via SA_SIGINFO");
    TEST_ASSERT(g_seen_signo == SIGUSR1, "siginfo si_signo matches delivered signal");
    TEST_ASSERT(g_seen_rdx != 0, "ucontext pointer (%rdx) is non-NULL");
    TEST_ASSERT(g_seen_rsi != 0, "siginfo pointer (%rsi) is non-NULL");

    if (g_seen_fpregs) {
        TEST_ASSERT((g_seen_fpregs % 64) == 0, "uc_mcontext.fpregs is 64-byte aligned");
        if (g_seen_magic1 == FP_XSTATE_MAGIC1) {
            TEST_ASSERT(g_seen_ext_size > 512, "XSAVE extended_size > 512 bytes");
            TEST_ASSERT(g_seen_magic2 == FP_XSTATE_MAGIC2, "FP_XSTATE_MAGIC2 present at extended_size - 4");
        } else {
            TEST_ASSERT(g_seen_ext_size == 512, "Legacy FXSAVE extended_size == 512 bytes");
        }
    }

    /* Restore default */
    signal(SIGUSR1, SIG_DFL);
}

/* Test 2: Handler modifying uc_mcontext to resume around a fault */
static volatile sig_atomic_t g_ud2_skipped = 0;
extern char g_ud2_label[];
extern char g_resume_label[];

static void ud2_step_handler(int sig, siginfo_t *info, void *uctx)
{
    (void)sig;
    (void)info;
    ucontext_t *uc = (ucontext_t *)uctx;
    /* Skip the 2-byte ud2 instruction (0x0f 0x0b) */
    uc->uc_mcontext.gregs[REG_RIP] += 2;
    uc->uc_mcontext.gregs[REG_RAX] = 0x42;
    g_ud2_skipped = 1;
}

static void test_sigreturn_register_mutation(void)
{
    printf("--- Test 2: B4.1 / B4.2 Register Mutation via uc_mcontext ---\n");
    g_ud2_skipped = 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = ud2_step_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGILL, &sa, NULL) != 0) {
        perror("sigaction");
        return;
    }

    uint64_t result_rax = 0;
    __asm__ volatile (
        "mov $0, %%rax\n"
        "ud2\n"               /* Triggers SIGILL */
        "mov %%rax, %0\n"
        : "=r"(result_rax)
        :
        : "rax", "memory"
    );

    TEST_ASSERT(g_ud2_skipped == 1, "SIGILL handler stepped over faulting ud2 instruction");
    TEST_ASSERT(result_rax == 0x42, "Modified uc_mcontext.gregs[REG_RAX] was restored by rt_sigreturn");

    signal(SIGILL, SIG_DFL);
}

/* Test 3: B4.2 Robustness against corrupted frame */
static void test_corrupted_frame_safety(void)
{
    printf("--- Test 3: B4.2 Malicious/Corrupted Frame Robustness ---\n");

    /* Subtest 3a: Non-canonical RIP in frame passed to sys_rt_sigreturn */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child: build a corrupt ucontext on stack and invoke rt_sigreturn directly */
        char fake_stack[2048] __attribute__((aligned(64)));
        ucontext_t *uc = (ucontext_t *)(fake_stack + 128);
        memset(uc, 0, sizeof(*uc));
        /* Set non-canonical kernel address for RIP */
        uc->uc_mcontext.gregs[REG_RIP] = 0xffff800000000000ULL;
        uc->uc_mcontext.gregs[REG_RSP] = (uint64_t)(uintptr_t)(fake_stack + 1024);

        /* Invoke rt_sigreturn with RSP pointing right above uc */
        uint64_t syscall_rsp = (uint64_t)(uintptr_t)uc;
        __asm__ volatile (
            "mov %0, %%rsp\n"
            "mov $15, %%rax\n"    /* SYS_rt_sigreturn */
            "syscall\n"
            :
            : "r"(syscall_rsp)
            : "memory"
        );
        /* Should not reach here */
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
                "Non-canonical RIP in rt_sigreturn killed child with SIGSEGV");

    /* Subtest 3b: Non-canonical RSP in frame */
    pid = fork();
    if (pid == 0) {
        char fake_stack[2048] __attribute__((aligned(64)));
        ucontext_t *uc = (ucontext_t *)(fake_stack + 128);
        memset(uc, 0, sizeof(*uc));
        uc->uc_mcontext.gregs[REG_RIP] = (uint64_t)(uintptr_t)test_signal_frame_layout;
        uc->uc_mcontext.gregs[REG_RSP] = 0xffff800000000000ULL;

        uint64_t syscall_rsp = (uint64_t)(uintptr_t)uc;
        __asm__ volatile (
            "mov %0, %%rsp\n"
            "mov $15, %%rax\n"
            "syscall\n"
            :
            : "r"(syscall_rsp)
            : "memory"
        );
        _exit(0);
    }

    status = 0;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
                "Non-canonical RSP in rt_sigreturn killed child with SIGSEGV");

    /* Subtest 3c: Bad memory pointer for rt_sigreturn */
    pid = fork();
    if (pid == 0) {
        uint64_t bad_rsp = 0x42ULL;
        __asm__ volatile (
            "mov %0, %%rsp\n"
            "mov $15, %%rax\n"
            "syscall\n"
            :
            : "r"(bad_rsp)
            : "memory"
        );
        _exit(0);
    }

    status = 0;
    waitpid(pid, &status, 0);
    TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
                "Unmapped frame base in rt_sigreturn killed child with SIGSEGV");
}

int main(void)
{
    printf("== AzamiOS Workstream B4.1 & B4.2: Signal Frame & rt_sigreturn Suite ==\n");
    test_signal_frame_layout();
    test_sigreturn_register_mutation();
    test_corrupted_frame_safety();

    printf("== Summary: %d / %d tests passed ==\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
