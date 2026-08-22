/* ============================================================================
 * AzamiOS Userspace — POSIX Process Spawn Implementation (spawn.c)
 * File: userland/libc/spawn.c
 * ============================================================================ */

#include "include/spawn.h"
#include "include/unistd.h"
#include "include/fcntl.h"
#include "include/stdlib.h"
#include "include/string.h"

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

static void apply_file_actions(const posix_spawn_file_actions_t *fa)
{
    if (!fa) return;
    for (int i = 0; i < fa->action_count; i++) {
        if (fa->actions[i].type == 0) { /* open */
            int fd = open(fa->actions[i].path, fa->actions[i].oflag, fa->actions[i].mode);
            if (fd >= 0 && fd != fa->actions[i].fd) {
                dup2(fd, fa->actions[i].fd);
                close(fd);
            }
        } else if (fa->actions[i].type == 1) { /* close */
            close(fa->actions[i].fd);
        } else if (fa->actions[i].type == 2) { /* dup2 */
            dup2(fa->actions[i].fd, fa->actions[i].newfd);
        }
    }
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[])
{
    if (!path) return 22;

    pid_t cpid = fork();
    if (cpid < 0) return 12; /* ENOMEM / EAGAIN */

    if (cpid == 0) {
        if (attrp && (attrp->flags & POSIX_SPAWN_SETPGROUP)) {
            setpgid(0, attrp->pgroup);
        }
        apply_file_actions(file_actions);
        execve(path, argv, envp);
        _exit(127);
    }

    if (pid) *pid = cpid;
    return 0;
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const argv[], char *const envp[])
{
    if (!file) return 22;

    pid_t cpid = fork();
    if (cpid < 0) return 12;

    if (cpid == 0) {
        if (attrp && (attrp->flags & POSIX_SPAWN_SETPGROUP)) {
            setpgid(0, attrp->pgroup);
        }
        apply_file_actions(file_actions);
        execvpe(file, argv, envp);
        _exit(127);
    }

    if (pid) *pid = cpid;
    return 0;
}
