/**
 * signal.h — AzamiOS libc: minimal signal stubs
 *
 * AzamiOS does not yet implement kernel-delivered signals;
 * these stubs provide POSIX-compatible signatures so code compiles.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>

/* Standard signal numbers */
#define SIGABRT  6
#define SIGFPE   8
#define SIGILL   4
#define SIGINT   2
#define SIGSEGV  11
#define SIGTERM  15

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

sighandler_t signal(int signum, sighandler_t handler);
int          raise (int signum);
int          kill  (pid_t pid, int signum);

#endif /* _SIGNAL_H */
