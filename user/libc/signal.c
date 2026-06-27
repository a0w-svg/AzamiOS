/**
 * signal.c — AzamiOS libc: minimal signal handling stubs
 */
#include "include/signal.h"
#include "include/stdio.h"
#include "include/stdlib.h"

static sighandler_t _handlers[32] = {0};

sighandler_t signal(int signum, sighandler_t handler) {
    if (signum < 0 || signum >= 32) return SIG_ERR;
    sighandler_t old = _handlers[signum];
    _handlers[signum] = handler;
    return old;
}

int raise(int signum) {
    if (signum < 0 || signum >= 32) return -1;
    sighandler_t h = _handlers[signum];
    if (h == SIG_IGN) return 0;
    if (h && h != SIG_DFL) {
        h(signum);
        return 0;
    }
    printf("Terminated by signal %d\n", signum);
    exit(128 + signum);
    return 0;
}

int kill(pid_t pid, int signum) {
    (void)pid;
    return raise(signum);
}

void _assert_fail(const char *expr, const char *file, int line, const char *func) {
    printf("Assertion failed: %s (%s: %s: %d)\n", expr, file, func, line);
    abort();
}
