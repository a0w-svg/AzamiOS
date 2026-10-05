#include <errno.h>
#include <stdio.h>
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
                       "AZAMI_SMOKE_TOKEN=loader-environment", NULL };
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
    puts("== AzamiOS BusyBox and dynamic loader smoke ==");
    char *shell[] = { "sh", "/etc/busybox-smoke.sh", NULL };
    char *native[] = { "/bin/native-dynamic-probe", "--smoke", NULL };
    char *musl[] = { "/bin/musl-dynamic-probe", "--smoke", NULL };
    run("BusyBox applet matrix", "/bin/sh", shell);
    run("ld-azami.so", native[0], native);
    /* Give each interpreter its matching ABI-compatible test libraries. */
    if (rename("/lib/libsmoke_start.so", "/lib/libsmoke_start-native.so") ||
        rename("/lib/libsmoke_plugin.so", "/lib/libsmoke_plugin-native.so") ||
        rename("/lib/libsmoke_start-musl.so", "/lib/libsmoke_start.so") ||
        rename("/lib/libsmoke_plugin-musl.so", "/lib/libsmoke_plugin.so")) {
        perror("select musl libraries");
        failed++;
    } else {
        run("ld-musl-x86_64.so.1", musl[0], musl);
    }
    printf("== probe complete: %d passed, %d failed ==\n", passed, failed);
    return failed ? 1 : 0;
}
