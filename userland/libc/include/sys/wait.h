/* ============================================================================
 * AzamiOS Userspace — Process Wait Header (sys/wait.h)
 * File: userland/libc/include/sys/wait.h
 * ============================================================================ */
#pragma once

#define WNOHANG   1
#define WUNTRACED 2

#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFSIGNALED(status) (((signed char)(((status) & 0x7f) + 1) >> 1) > 0)
#define WTERMSIG(status)    ((status) & 0x7f)

int wait(int *status);
int waitpid(int pid, int *status, int options);
