/* ============================================================================
 * AzamiOS Userspace — Pseudo-Terminal Control Implementation (pty.c)
 * File: userland/libc/pty.c
 * ============================================================================ */

#include "include/pty.h"
#include "include/fcntl.h"
#include "include/unistd.h"
#include "include/sys/ioctl.h"
#include "include/termios.h"
#include "include/stdio.h"
#include "include/string.h"
#include "include/errno.h"
#include "include/sys/stat.h"

int grantpt(int fd)
{
    char buf[64];
    if (ptsname_r(fd, buf, sizeof(buf)) != 0) {
        return -1;
    }
    return chmod(buf, 0620);
}

int unlockpt(int fd)
{
    int unlock = 0;
    return ioctl(fd, TIOCSPTLCK, (unsigned long)&unlock);
}

int ptsname_r(int fd, char *buf, size_t buflen)
{
    if (!buf) {
        errno = EINVAL;
        return EINVAL;
    }
    int pty_num = -1;
    if (ioctl(fd, TIOCGPTN, (unsigned long)&pty_num) < 0) {
        return errno ? errno : ENOTTY;
    }
    int n = snprintf(buf, buflen, "/dev/pts/%d", pty_num);
    if (n < 0 || (size_t)n >= buflen) {
        errno = ERANGE;
        return ERANGE;
    }
    return 0;
}

static char s_ptsname_buf[64];
char *ptsname(int fd)
{
    if (ptsname_r(fd, s_ptsname_buf, sizeof(s_ptsname_buf)) != 0) {
        return NULL;
    }
    return s_ptsname_buf;
}

int login_tty(int fd)
{
    (void)setsid();
    if (ioctl(fd, TIOCSCTTY, 0) < 0) {
        /* Continue even if ioctl failed (e.g. already controlling) */
    }
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2) {
        close(fd);
    }
    return 0;
}

int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp,
            const struct winsize *winp)
{
    if (!amaster || !aslave) {
        errno = EINVAL;
        return -1;
    }

    int master = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (master < 0) {
        master = open("/dev/pts/ptmx", O_RDWR | O_NOCTTY);
    }
    if (master < 0) {
        return -1;
    }

    if (grantpt(master) < 0 || unlockpt(master) < 0) {
        close(master);
        return -1;
    }

    char sname[64];
    if (ptsname_r(master, sname, sizeof(sname)) != 0) {
        close(master);
        return -1;
    }

    if (name) {
        strcpy(name, sname);
    }

    int slave = open(sname, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return -1;
    }

    if (termp) {
        tcsetattr(slave, TCSANOW, termp);
    }
    if (winp) {
        ioctl(slave, TIOCSWINSZ, (unsigned long)winp);
    }

    *amaster = master;
    *aslave = slave;
    return 0;
}

pid_t forkpty(int *amaster, char *name,
              const struct termios *termp,
              const struct winsize *winp)
{
    int master = -1, slave = -1;
    if (openpty(&master, &slave, name, termp, winp) < 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(master);
        if (login_tty(slave) < 0) {
            _exit(1);
        }
        return 0;
    }

    /* Parent */
    close(slave);
    if (amaster) {
        *amaster = master;
    } else {
        close(master);
    }
    return pid;
}
