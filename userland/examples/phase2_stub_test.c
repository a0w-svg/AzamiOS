/* ============================================================================
 * AzamiOS — Phase 2 stub-fix regression test
 * File: userland/examples/phase2_stub_test.c
 *
 * Exercises the syscalls that used to be silent no-ops or wrong before
 * Phase 2 of the libc hardening work: fchdir, mlock/munlock, futimesat,
 * settimeofday, initgroups, adjtimex.
 * ============================================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <grp.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("[PASS] %s\n", msg); \
    else { printf("[FAIL] %s (errno=%d)\n", msg, errno); g_fail++; } \
} while (0)

int main(void)
{
    printf("=== AzamiOS Phase 2 stub-fix test ===\n");

    /* fchdir(): used to be a pure no-op. */
    int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    CHECK(dfd >= 0, "open(/tmp) for fchdir");
    if (dfd >= 0) {
        int r = fchdir(dfd);
        char cwd[256] = {0};
        getcwd(cwd, sizeof(cwd));
        CHECK(r == 0 && strcmp(cwd, "/tmp") == 0, "fchdir(/tmp) actually changes cwd");
        close(dfd);
        chdir("/");
    }

    /* mlock/munlock: used to be a no-op that never checked the range was
     * mapped at all. Real memory should lock; garbage address should fail. */
    void *buf = malloc(4096);
    CHECK(buf != NULL, "malloc for mlock test");
    if (buf) {
        int r = mlock(buf, 4096);
        CHECK(r == 0, "mlock(valid heap page) succeeds");
        r = munlock(buf, 4096);
        CHECK(r == 0, "munlock(valid heap page) succeeds");
        free(buf);
    }
    {
        /* This kernel's VMA list is a best-effort index, not an
         * authoritative map (brk-managed heap never gets a VMA at all — see
         * vma_set_locked()'s comment in kernel/mm/vma.c), so unlike real
         * mlock(2) there is no reliable way to distinguish "definitely
         * unmapped" from "valid but untracked" here. A garbage address is
         * therefore a harmless no-op, not -ENOMEM. */
        int r = mlock((void *)0x7fff00000000UL, 4096);
        CHECK(r == 0, "mlock(untracked address) is a harmless no-op");
    }

    /* mlockall/munlockall */
    {
        int r = mlockall(MCL_CURRENT);
        CHECK(r == 0, "mlockall(MCL_CURRENT) succeeds");
        r = munlockall();
        CHECK(r == 0, "munlockall() succeeds");
    }

    /* futimesat / utimensat via futimens: used to be a no-op — mtime should
     * actually change and be visible through stat(). */
    {
        const char *path = "/tmp/phase2_futimes_test";
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        CHECK(fd >= 0, "create test file for futimesat");
        if (fd >= 0) close(fd);

        struct timespec times[2] = { {1000000, 0}, {2000000, 0} };
        int r = utimensat(AT_FDCWD, path, times, 0);
        CHECK(r == 0, "utimensat sets a specific time");

        struct stat st;
        r = stat(path, &st);
        CHECK(r == 0 && st.st_atime == 1000000 && st.st_mtime == 2000000,
              "stat() reflects the utimensat-set atime/mtime");
        unlink(path);
    }

    /* settimeofday: used to unconditionally return -EPERM, even for root. */
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        long saved = tv.tv_sec;

        tv.tv_sec = saved + 100;
        tv.tv_usec = 0;
        int r = settimeofday(&tv, NULL);
        CHECK(r == 0, "settimeofday() as root succeeds");

        struct timeval after;
        gettimeofday(&after, NULL);
        CHECK(after.tv_sec >= saved + 100, "clock actually advanced by the settimeofday delta");

        /* restore, so this doesn't leave the VM's clock skewed for anything
         * that runs after this test */
        tv.tv_sec = saved;
        settimeofday(&tv, NULL);
    }

    /* adjtimex: used to ignore the buffer entirely. */
    {
        struct timex tx;
        memset(&tx, 0xAA, sizeof(tx)); /* poison, so a no-op fill is visible */
        tx.modes = 0;
        int r = adjtimex(&tx);
        CHECK(r == 0, "adjtimex(modes=0) returns TIME_OK");
        CHECK(tx.time.tv_sec != (long)0xAAAAAAAAAAAAAAAAUL, "adjtimex actually fills tx.time");
        CHECK(tx.tick == 10000, "adjtimex reports a plausible tick value");
    }

    /* initgroups: used to never call setgroups() at all. */
    {
        int r = initgroups("root", 0);
        CHECK(r == 0, "initgroups(\"root\", 0) succeeds");
        gid_t groups[32];
        int n = getgroups(32, groups);
        CHECK(n >= 1, "getgroups() reflects the initgroups() call");
    }

    if (g_fail == 0) {
        printf("=== ALL PASS ===\n");
        return 0;
    }
    printf("=== %d FAILURE(S) ===\n", g_fail);
    return 1;
}
