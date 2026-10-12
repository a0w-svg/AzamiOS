#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int passed, failed;

static void run(const char *name, const char *path, char *const argv[])
{
    printf("-- %s --\n", name);
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        char *env[] = { "PATH=/bin:/usr/bin:/usr/sbin", "HOME=/root",
                        "AZAMI_SMOKE_TOKEN=ports-environment", NULL };
        execve(path, argv, env);
        perror(path);
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    if (child < 0) {
        perror("fork");
        failed++;
        return;
    }
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS: %s (exit 0)\n", name);
        passed++;
    } else {
        printf("FAIL: %s (wait=%d status=%#x)\n", name, (int)waited, status);
        failed++;
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("== AzamiOS Stock Linux Ports Matrix ==");

    char *shell[] = { "sh", "/etc/ports-smoke.sh", NULL };
    run("Port Smoke Matrix Script", "/bin/sh", shell);

    /* Trigger an unimplemented syscall to verify telemetry recording */
    long enosys_ret = syscall(9999);
    if (enosys_ret == -1 && errno == ENOSYS) {
        printf("PASS: unhandled syscall returned -ENOSYS as expected\n");
        passed++;
    } else {
        printf("FAIL: expected -ENOSYS from syscall 9999, got ret=%ld errno=%d\n", enosys_ret, errno);
        failed++;
    }

    /* Direct verification of /proc/azami/enosys telemetry endpoint */
    int fd = open("/proc/azami/enosys", O_RDONLY);
    if (fd >= 0) {
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n >= 0) {
            buf[n] = '\0';
            printf("Direct /proc/azami/enosys read (%zd bytes):\n%s\n", n, buf);
            if (strstr(buf, "9999") != NULL) {
                printf("PASS: /proc/azami/enosys contains recorded syscall 9999\n");
                passed++;
            } else {
                printf("FAIL: /proc/azami/enosys missing syscall 9999\n");
                failed++;
            }
        } else {
            perror("read /proc/azami/enosys");
            failed++;
        }
        close(fd);
    } else {
        perror("open /proc/azami/enosys");
        failed++;
    }

    /* musl's posix_spawn is clone(CLONE_VM|CLONE_VFORK) on a private stack. */
    {
        char *sargv[] = { "sh", "-c", "exit 5", NULL };
        char *senv[] = { "PATH=/bin", NULL };
        pid_t sp = 0;
        int rc = posix_spawn(&sp, "/bin/sh", NULL, NULL, sargv, senv);
        int st = 0;
        if (rc == 0 && waitpid(sp, &st, 0) == sp && WIFEXITED(st) && WEXITSTATUS(st) == 5) {
            printf("PASS: posix_spawn CLONE_VM|CLONE_VFORK child exit status\n");
            passed++;
        } else {
            printf("FAIL: posix_spawn rc=%d st=%#x\n", rc, st);
            failed++;
        }

        rc = posix_spawn(&sp, "/nonexistent/binary", NULL, NULL, sargv, senv);
        if (rc == ENOENT) {
            printf("PASS: posix_spawn reports ENOENT for missing binary\n");
            passed++;
        } else {
            printf("FAIL: posix_spawn missing binary rc=%d\n", rc);
            failed++;
        }

        /* Parent stack must be intact and parent must resume only after the
         * child released it (exec/exit). */
        volatile int shared = 1;
        pid_t vp = vfork();
        if (vp == 0) {
            shared = 42;
            _exit(9);
        }
        if (vp > 0 && waitpid(vp, &st, 0) == vp && WEXITSTATUS(st) == 9 && shared == 42) {
            printf("PASS: vfork shares memory and parent resumes after child exit\n");
            passed++;
        } else {
            printf("FAIL: vfork vp=%d shared=%d st=%#x\n", (int)vp, shared, st);
            failed++;
        }
    }

    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
