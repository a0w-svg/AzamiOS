/* ============================================================================
 * AzamiOS Userspace — POSIX Process Spawn Implementation (spawn.c)
 * File: userland/libc/spawn.c
 * ============================================================================ */

#include "include/spawn.h"
#include "include/unistd.h"
#include "include/fcntl.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/signal.h"
#include "include/errno.h"
#include "include/sched.h"
#include "include/sys/mman.h"
#include "include/sys/wait.h"

int posix_spawn_file_actions_init(posix_spawn_file_actions_t *file_actions)
{
    if (!file_actions) return 22; /* EINVAL */
    file_actions->action_count = 0;
    return 0;
}

int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *file_actions)
{
    if (!file_actions) return 22;
    file_actions->action_count = 0;
    return 0;
}

int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *file_actions,
                                     int fd, const char *path, int oflag, mode_t mode)
{
    if (!file_actions || file_actions->action_count >= 16 || !path) return 22;
    int idx = file_actions->action_count++;
    file_actions->actions[idx].type = 0; /* open */
    file_actions->actions[idx].fd = fd;
    strncpy(file_actions->actions[idx].path, path, 255);
    file_actions->actions[idx].path[255] = '\0';
    file_actions->actions[idx].oflag = oflag;
    file_actions->actions[idx].mode = mode;
    return 0;
}

int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *file_actions, int fd)
{
    if (!file_actions || file_actions->action_count >= 16) return 22;
    int idx = file_actions->action_count++;
    file_actions->actions[idx].type = 1; /* close */
    file_actions->actions[idx].fd = fd;
    return 0;
}

int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *file_actions, int fd, int newfd)
{
    if (!file_actions || file_actions->action_count >= 16) return 22;
    int idx = file_actions->action_count++;
    file_actions->actions[idx].type = 2; /* dup2 */
    file_actions->actions[idx].fd = fd;
    file_actions->actions[idx].newfd = newfd;
    return 0;
}

int posix_spawnattr_init(posix_spawnattr_t *attr)
{
    if (!attr) return 22;
    attr->flags = 0;
    attr->pgroup = 0;
    return 0;
}

int posix_spawnattr_destroy(posix_spawnattr_t *attr)
{
    (void)attr;
    return 0;
}

int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags)
{
    if (!attr) return 22;
    attr->flags = flags;
    return 0;
}

int posix_spawnattr_getflags(const posix_spawnattr_t *attr, short *flags)
{
    if (!attr || !flags) return 22;
    *flags = (short)attr->flags;
    return 0;
}

int posix_spawnattr_getpgroup(const posix_spawnattr_t *attr, pid_t *pgroup)
{
    if (!attr || !pgroup) return 22;
    *pgroup = (pid_t)attr->pgroup;
    return 0;
}

int posix_spawnattr_setpgroup(posix_spawnattr_t *attr, pid_t pgroup)
{
    if (!attr) return 22;
    attr->pgroup = (short)pgroup;
    return 0;
}

int posix_spawnattr_getsigmask(const posix_spawnattr_t *attr, sigset_t *sigmask)
{
    if (!attr || !sigmask) return 22;
    *sigmask = attr->sigmask;
    return 0;
}

int posix_spawnattr_setsigmask(posix_spawnattr_t *attr, const sigset_t *sigmask)
{
    if (!attr || !sigmask) return 22;
    attr->sigmask = *sigmask;
    return 0;
}

int posix_spawnattr_getsigdefault(const posix_spawnattr_t *attr, sigset_t *sigdefault)
{
    if (!attr || !sigdefault) return 22;
    *sigdefault = attr->sigdefault;
    return 0;
}

int posix_spawnattr_setsigdefault(posix_spawnattr_t *attr, const sigset_t *sigdefault)
{
    if (!attr || !sigdefault) return 22;
    attr->sigdefault = *sigdefault;
    return 0;
}

static int apply_spawn_attr(const posix_spawnattr_t *attrp)
{
    if (!attrp) return 0;
    if (attrp->flags & POSIX_SPAWN_SETPGROUP) {
        if (setpgid(0, attrp->pgroup) < 0) return errno;
    }
    if (attrp->flags & POSIX_SPAWN_RESETIDS) {
        if (seteuid(getuid()) < 0) return errno;
        if (setegid(getgid()) < 0) return errno;
    }
    if (attrp->flags & POSIX_SPAWN_SETSIGMASK) {
        if (sigprocmask(SIG_SETMASK, &attrp->sigmask, NULL) < 0) return errno;
    }
    if (attrp->flags & POSIX_SPAWN_SETSIGDEF) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        for (int sig = 1; sig < 32; sig++) {
            if (attrp->sigdefault & (1UL << (sig - 1))) {
                if (sigaction(sig, &sa, NULL) < 0) return errno;
            }
        }
    }
    return 0;
}

static int apply_file_actions(const posix_spawn_file_actions_t *fa)
{
    if (!fa) return 0;
    for (int i = 0; i < fa->action_count; i++) {
        if (fa->actions[i].type == 0) { /* open */
            int fd = open(fa->actions[i].path, fa->actions[i].oflag, fa->actions[i].mode);
            if (fd < 0) return errno;
            if (fd != fa->actions[i].fd) {
                if (dup2(fd, fa->actions[i].fd) < 0) { int e = errno; close(fd); return e; }
                close(fd);
            }
        } else if (fa->actions[i].type == 1) { /* close */
            if (close(fa->actions[i].fd) < 0) return errno;
        } else if (fa->actions[i].type == 2) { /* dup2 */
            if (dup2(fa->actions[i].fd, fa->actions[i].newfd) < 0) return errno;
        }
    }
    return 0;
}

/* Everything the vfork-style child needs. It lives in the parent's frame,
 * which is safe because the parent stays blocked inside clone() until the
 * child execs or exits; `err` is how the child reports a failure back. */
struct spawn_ctx {
    const char *path;
    int use_path;
    const posix_spawn_file_actions_t *fa;
    const posix_spawnattr_t *attr;
    char *const *argv;
    char *const *envp;
    volatile int err;
};

#define SPAWN_STACK_SIZE (64 * 1024)

static int spawn_child(void *p)
{
    struct spawn_ctx *c = (struct spawn_ctx *)p;
    int e = apply_spawn_attr(c->attr);
    if (!e) e = apply_file_actions(c->fa);
    if (!e) {
        if (c->use_path) execvpe(c->path, c->argv, c->envp);
        else             execve(c->path, c->argv, c->envp);
        e = errno ? errno : 8; /* ENOEXEC */
    }
    c->err = e;
    _exit(127);
}

static int do_spawn(pid_t *pid, const char *path, int use_path,
                    const posix_spawn_file_actions_t *fa,
                    const posix_spawnattr_t *attrp,
                    char *const argv[], char *const envp[])
{
    void *stack = mmap(NULL, SPAWN_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return errno ? errno : 12;

    struct spawn_ctx ctx = { path, use_path, fa, attrp, argv, envp, 0 };
    int cpid = clone(spawn_child, (char *)stack + SPAWN_STACK_SIZE,
                     CLONE_VM | CLONE_VFORK | 17 /* SIGCHLD */, &ctx);
    int clone_errno = errno;
    munmap(stack, SPAWN_STACK_SIZE);
    if (cpid < 0) return clone_errno ? clone_errno : 12;

    if (ctx.err) {
        int status;
        waitpid(cpid, &status, 0);
        return ctx.err;
    }
    if (pid) *pid = cpid;
    return 0;
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[])
{
    if (!path) return 22;
    return do_spawn(pid, path, 0, file_actions, attrp, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[])
{
    if (!file) return 22;
    return do_spawn(pid, file, 1, file_actions, attrp, argv, envp);
}
