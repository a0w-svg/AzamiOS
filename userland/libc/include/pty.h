/* ============================================================================
 * AzamiOS Userspace — Pseudo-Terminal Control Header (pty.h)
 * File: userland/libc/include/pty.h
 * ============================================================================ */
#pragma once

#include "termios.h"
#include "sys/ioctl.h"
#include "sys/types.h"

int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp,
            const struct winsize *winp);

pid_t forkpty(int *amaster, char *name,
              const struct termios *termp,
              const struct winsize *winp);

int login_tty(int fd);

int grantpt(int fd);
int unlockpt(int fd);
char *ptsname(int fd);
int ptsname_r(int fd, char *buf, size_t buflen);
