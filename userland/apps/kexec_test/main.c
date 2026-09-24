/* ============================================================================
 * AzamiOS — kexec smoke test
 * File: userland/apps/kexec_test/main.c
 *
 * Exercises the real kexec_file_load(2) / reboot(2, RB_KEXEC) implementation
 * (kernel/kexec.c): stages the currently-running kernel's own ELF (copied to
 * /boot/kernel.elf by every build — see scripts/create_disk.py) as the
 * "next" kernel, then actually jumps into it via reboot(RB_KEXEC).
 *
 * A handful of negative cases run first and must never touch the running
 * kernel at all (kexec_load() validation happens well before anything
 * irreversible). The final call is the real thing: on success this process,
 * and everything else running, simply stops existing mid-sentence as the
 * new kernel instance starts booting from scratch — the strongest possible
 * proof is a second "AzamiOS kernel starting..." banner appearing in the
 * same serial log, with no QEMU restart in between.
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/reboot.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s (errno=%d)\n", msg, errno); g_fail++; } \
} while (0)

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("=== AzamiOS kexec smoke test ===\n");

    /* ---- Negative cases: none of these may affect the running kernel. ---- */
    {
        int r = kexec_file_load(-1, -1, 0, NULL, 0);
        CHECK(r < 0 && errno == EBADF, "kexec_file_load(bad kernel_fd) -> EBADF");
    }

    int kfd = open("/boot/kernel.elf", O_RDONLY);
    CHECK(kfd >= 0, "open(/boot/kernel.elf)");
    if (kfd < 0) {
        printf("=== %d FAILURE(S) -- cannot continue without the kernel image ===\n",
               g_fail);
        return 1;
    }

    /* This test is gated by /etc/run-kexec-test (see userland/apps/init/
     * main.c) so it never runs on an ordinary boot. Removing the marker
     * here, before the real jump below, means the kernel instance this
     * jumps *into* will not re-run it: this test validates a single kexec
     * (load a fresh copy, jump into it), not an unbounded chain of them --
     * kexec_execute()'s own memmap forging has a documented limitation
     * around exactly that (see kernel/kexec.c), so deliberately not
     * exercising it here is the correct scope, not a workaround. */
    unlink("/etc/run-kexec-test");

    {
        /* A real (non -1) initrd fd must be rejected: this kernel has no
         * initrd boot mechanism at all. */
        int r = kexec_file_load(kfd, kfd, 0, NULL, 0);
        CHECK(r < 0 && errno == EINVAL, "kexec_file_load(real initrd_fd) -> EINVAL");
    }

    {
        /* No KEXEC_FILE_* flag is implemented; a nonzero flags word must be
         * a real error, not a silently-ignored no-op. */
        int r = kexec_file_load(kfd, -1, 0, NULL, 1);
        CHECK(r < 0 && errno == EINVAL, "kexec_file_load(nonzero flags) -> EINVAL");
    }

    {
        /* A non-ELF file must be rejected as a malformed image. */
        int bad = open("/etc/hostname", O_RDONLY);
        if (bad < 0) bad = open("/tmp", O_RDONLY);
        if (bad >= 0) {
            int r = kexec_file_load(bad, -1, 0, NULL, 0);
            CHECK(r < 0, "kexec_file_load(non-ELF file) is rejected");
            close(bad);
        } else {
            printf("[SKIP] no convenient non-ELF file found to test rejection with\n");
        }
    }

    /* ---- Happy path: stage the currently-running kernel's own ELF. ---- */
    const char *cmdline = "kexec_test=1";
    int r = kexec_file_load(kfd, -1, strlen(cmdline) + 1, cmdline, 0);
    CHECK(r == 0, "kexec_file_load(/boot/kernel.elf) succeeds");
    close(kfd);

    if (r != 0) {
        printf("=== %d FAILURE(S) ===\n", g_fail);
        return 1;
    }

    if (g_fail != 0) {
        printf("=== %d earlier FAILURE(S) -- attempting the real jump anyway ===\n", g_fail);
    }

    printf("[KEXEC_TEST] Image staged. Calling reboot(RB_KEXEC) now -- a second "
           "AzamiOS boot banner appearing right after this line, with no QEMU "
           "restart, is the proof this actually worked.\n");
    fflush(stdout);

    int rr = reboot(RB_KEXEC);

    /* Only reached if the kexec attempt failed -- success never returns. */
    printf("[FAIL] reboot(RB_KEXEC) returned (rc=%d, errno=%d) -- kexec did not happen\n",
           rr, errno);
    return 1;
}
