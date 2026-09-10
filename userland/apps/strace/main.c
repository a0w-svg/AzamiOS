/* ============================================================================
 * AzamiOS — strace: System Call Tracer
 * File: userland/apps/strace/main.c
 *
 * Capabilities
 * ────────────
 *  • Trace execution of new process: strace <program> [args...]
 *  • Trace already-running process: strace -p <pid>
 *  • Count syscalls and timing summary: strace -c <program> [args...]
 *  • Output redirection: strace -o <filename> ...
 *  • String argument dereferencing via PTRACE_PEEKDATA (paths, buffers)
 *  • Decodes return values and errno symbols
 *  • Tracks and prints signal delivery events
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/syscall.h>

#define MAX_STR_LEN 64

/* ── Syscall Argument Types ───────────────────────────────────────────────── */
typedef enum {
    ARG_NONE = 0,
    ARG_INT,
    ARG_HEX,
    ARG_PTR,
    ARG_STR,
    ARG_FD,
    ARG_OCT
} arg_type_t;

typedef struct {
    int         nr;
    const char *name;
    int         nargs;
    arg_type_t  arg_types[6];
} syscall_desc_t;

/* ── Linux/AzamiOS x86_64 System Call Table ───────────────────────────────── */
static const syscall_desc_t g_syscalls[] = {
    { 0,   "read",            3, { ARG_FD,  ARG_PTR, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 1,   "write",           3, { ARG_FD,  ARG_STR, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 2,   "open",            3, { ARG_STR, ARG_HEX, ARG_OCT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 3,   "close",           1, { ARG_FD,  ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 4,   "stat",            2, { ARG_STR, ARG_PTR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 5,   "fstat",           2, { ARG_FD,  ARG_PTR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 6,   "lstat",           2, { ARG_STR, ARG_PTR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 7,   "poll",            3, { ARG_PTR, ARG_INT, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 8,   "lseek",           3, { ARG_FD,  ARG_INT, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 9,   "mmap",            6, { ARG_PTR, ARG_INT, ARG_HEX, ARG_HEX,  ARG_FD,   ARG_HEX } },
    { 10,  "mprotect",        3, { ARG_PTR, ARG_INT, ARG_HEX, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 11,  "munmap",          2, { ARG_PTR, ARG_INT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 12,  "brk",             1, { ARG_PTR, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 13,  "rt_sigaction",    4, { ARG_INT, ARG_PTR, ARG_PTR, ARG_INT,  ARG_NONE, ARG_NONE } },
    { 14,  "rt_sigprocmask",  4, { ARG_INT, ARG_PTR, ARG_PTR, ARG_INT,  ARG_NONE, ARG_NONE } },
    { 15,  "rt_sigreturn",    0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 16,  "ioctl",           3, { ARG_FD,  ARG_HEX, ARG_PTR, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 20,  "writev",          3, { ARG_FD,  ARG_PTR, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 21,  "access",          2, { ARG_STR, ARG_OCT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 22,  "pipe",            1, { ARG_PTR, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 23,  "select",          5, { ARG_INT, ARG_PTR, ARG_PTR, ARG_PTR,  ARG_PTR,  ARG_NONE } },
    { 24,  "sched_yield",     0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 32,  "dup",             1, { ARG_FD,  ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 33,  "dup2",            2, { ARG_FD,  ARG_FD,  ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 34,  "pause",           0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 35,  "nanosleep",       2, { ARG_PTR, ARG_PTR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 39,  "getpid",          0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 41,  "socket",          3, { ARG_INT, ARG_INT, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 42,  "connect",         3, { ARG_FD,  ARG_PTR, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 43,  "accept",          3, { ARG_FD,  ARG_PTR, ARG_PTR, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 44,  "sendto",          6, { ARG_FD,  ARG_PTR, ARG_INT, ARG_HEX,  ARG_PTR,  ARG_INT } },
    { 45,  "recvfrom",        6, { ARG_FD,  ARG_PTR, ARG_INT, ARG_HEX,  ARG_PTR,  ARG_PTR } },
    { 49,  "bind",            3, { ARG_FD,  ARG_PTR, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 50,  "listen",          2, { ARG_FD,  ARG_INT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 56,  "clone",           5, { ARG_HEX, ARG_PTR, ARG_PTR, ARG_PTR,  ARG_PTR,  ARG_NONE } },
    { 57,  "fork",            0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 58,  "vfork",           0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 59,  "execve",          3, { ARG_STR, ARG_PTR, ARG_PTR, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 60,  "exit",            1, { ARG_INT, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 61,  "wait4",           4, { ARG_INT, ARG_PTR, ARG_INT, ARG_PTR,  ARG_NONE, ARG_NONE } },
    { 62,  "kill",            2, { ARG_INT, ARG_INT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 63,  "uname",           1, { ARG_PTR, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 72,  "fcntl",           3, { ARG_FD,  ARG_HEX, ARG_HEX, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 77,  "ftruncate",       2, { ARG_FD,  ARG_INT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 79,  "getcwd",          2, { ARG_PTR, ARG_INT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 80,  "chdir",           1, { ARG_STR, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 83,  "mkdir",           2, { ARG_STR, ARG_OCT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 84,  "rmdir",           1, { ARG_STR, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 86,  "link",            2, { ARG_STR, ARG_STR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 87,  "unlink",          1, { ARG_STR, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 88,  "symlink",         2, { ARG_STR, ARG_STR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 89,  "readlink",        3, { ARG_STR, ARG_PTR, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 90,  "chmod",           2, { ARG_STR, ARG_OCT, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 92,  "chown",           3, { ARG_STR, ARG_INT, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 101, "ptrace",          4, { ARG_HEX, ARG_INT, ARG_PTR, ARG_PTR,  ARG_NONE, ARG_NONE } },
    { 102, "getuid",          0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 104, "getgid",          0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 107, "geteuid",         0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 108, "getegid",         0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 110, "getppid",         0, { ARG_NONE,ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 137, "statfs",          2, { ARG_STR, ARG_PTR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 138, "fstatfs",         2, { ARG_FD,  ARG_PTR, ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 217, "getdents64",      3, { ARG_FD,  ARG_PTR, ARG_INT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 231, "exit_group",      1, { ARG_INT, ARG_NONE,ARG_NONE,ARG_NONE, ARG_NONE, ARG_NONE } },
    { 257, "openat",          4, { ARG_FD,  ARG_STR, ARG_HEX, ARG_OCT,  ARG_NONE, ARG_NONE } },
    { 258, "mkdirat",         3, { ARG_FD,  ARG_STR, ARG_OCT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 263, "unlinkat",        3, { ARG_FD,  ARG_STR, ARG_HEX, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 267, "readlinkat",      4, { ARG_FD,  ARG_STR, ARG_PTR, ARG_INT,  ARG_NONE, ARG_NONE } },
    { 268, "fchmodat",        3, { ARG_FD,  ARG_STR, ARG_OCT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 269, "faccessat",       3, { ARG_FD,  ARG_STR, ARG_OCT, ARG_NONE, ARG_NONE, ARG_NONE } },
    { 332, "statx",           5, { ARG_FD,  ARG_STR, ARG_HEX, ARG_HEX,  ARG_PTR,  ARG_NONE } },
};

#define NUM_SYSCALLS (sizeof(g_syscalls) / sizeof(g_syscalls[0]))

static const syscall_desc_t *find_syscall(int nr)
{
    for (size_t i = 0; i < NUM_SYSCALLS; i++) {
        if (g_syscalls[i].nr == nr) return &g_syscalls[i];
    }
    return NULL;
}

/* ── Errno Names ──────────────────────────────────────────────────────────── */
static const char *errno_name(int err)
{
    switch (err) {
        case EPERM:   return "EPERM (Operation not permitted)";
        case ENOENT:  return "ENOENT (No such file or directory)";
        case ESRCH:   return "ESRCH (No such process)";
        case EINTR:   return "EINTR (Interrupted system call)";
        case EIO:     return "EIO (Input/output error)";
        case ENXIO:   return "ENXIO (No such device or address)";
        case E2BIG:   return "E2BIG (Argument list too long)";
        case ENOEXEC: return "ENOEXEC (Exec format error)";
        case EBADF:   return "EBADF (Bad file descriptor)";
        case ECHILD:  return "ECHILD (No child processes)";
        case EAGAIN:  return "EAGAIN (Resource temporarily unavailable)";
        case ENOMEM:  return "ENOMEM (Cannot allocate memory)";
        case EACCES:  return "EACCES (Permission denied)";
        case EFAULT:  return "EFAULT (Bad address)";
        case EBUSY:   return "EBUSY (Device or resource busy)";
        case EEXIST:  return "EEXIST (File exists)";
        case EXDEV:   return "EXDEV (Invalid cross-device link)";
        case ENODEV:  return "ENODEV (No such device)";
        case ENOTDIR: return "ENOTDIR (Not a directory)";
        case EISDIR:  return "EISDIR (Is a directory)";
        case EINVAL:  return "EINVAL (Invalid argument)";
        case ENFILE:  return "ENFILE (Too many open files in system)";
        case EMFILE:  return "EMFILE (Too many open files)";
        case ENOTTY:  return "ENOTTY (Inappropriate ioctl for device)";
        case EFBIG:   return "EFBIG (File too large)";
        case ENOSPC:  return "ENOSPC (No space left on device)";
        case ESPIPE:  return "ESPIPE (Illegal seek)";
        case EROFS:   return "EROFS (Read-only file system)";
        case EMLINK:  return "EMLINK (Too many links)";
        case EPIPE:   return "EPIPE (Broken pipe)";
        case ENOSYS:  return "ENOSYS (Function not implemented)";
        default:      return "ERROR";
    }
}

/* ── Signal Names ─────────────────────────────────────────────────────────── */
static const char *signal_name(int sig)
{
    switch (sig) {
        case SIGHUP:  return "SIGHUP";
        case SIGINT:  return "SIGINT";
        case SIGQUIT: return "SIGQUIT";
        case SIGILL:  return "SIGILL";
        case SIGTRAP: return "SIGTRAP";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGFPE:  return "SIGFPE";
        case SIGKILL: return "SIGKILL";
        case SIGUSR1: return "SIGUSR1";
        case SIGSEGV: return "SIGSEGV";
        case SIGUSR2: return "SIGUSR2";
        case SIGPIPE: return "SIGPIPE";
        case SIGALRM: return "SIGALRM";
        case SIGTERM: return "SIGTERM";
        case SIGCHLD: return "SIGCHLD";
        case SIGCONT: return "SIGCONT";
        case SIGSTOP: return "SIGSTOP";
        case SIGTSTP: return "SIGTSTP";
        default:      return "SIGNAL";
    }
}

/* ── Read string from tracee memory ───────────────────────────────────────── */
static void peek_string(pid_t pid, unsigned long addr, char *out, size_t maxlen)
{
    if (addr == 0) {
        snprintf(out, maxlen, "NULL");
        return;
    }

    size_t count = 0;
    int truncated = 0;
    out[0] = '"';
    count = 1;

    while (count < maxlen - 2) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid, (void *)(addr + count - 1), NULL);
        if (errno != 0) {
            snprintf(out, maxlen, "%p", (void *)addr);
            return;
        }

        char *bytes = (char *)&word;
        for (int i = 0; i < (int)sizeof(long) && count < maxlen - 2; i++) {
            char c = bytes[i];
            if (c == '\0') {
                out[count++] = '"';
                out[count] = '\0';
                return;
            }
            if (c == '\n') {
                if (count + 2 >= maxlen - 2) break;
                out[count++] = '\\'; out[count++] = 'n';
            } else if (c == '\t') {
                if (count + 2 >= maxlen - 2) break;
                out[count++] = '\\'; out[count++] = 't';
            } else if (c == '"' || c == '\\') {
                if (count + 2 >= maxlen - 2) break;
                out[count++] = '\\'; out[count++] = c;
            } else if (c >= 32 && c <= 126) {
                out[count++] = c;
            } else {
                out[count++] = '?';
            }
        }
        if (count >= maxlen - 6) {
            truncated = 1;
            break;
        }
    }

    if (truncated) {
        out[count++] = '"';
        out[count++] = '.';
        out[count++] = '.';
        out[count++] = '.';
        out[count] = '\0';
    } else {
        out[count++] = '"';
        out[count] = '\0';
    }
}

/* ── Format single argument ───────────────────────────────────────────────── */
static void format_arg(pid_t pid, arg_type_t type, unsigned long val, char *buf, size_t maxlen)
{
    switch (type) {
        case ARG_STR:
            peek_string(pid, val, buf, maxlen);
            break;
        case ARG_FD:
            if ((long)val == -100) { /* AT_FDCWD */
                snprintf(buf, maxlen, "AT_FDCWD");
            } else {
                snprintf(buf, maxlen, "%ld", (long)val);
            }
            break;
        case ARG_OCT:
            snprintf(buf, maxlen, "0%lo", val);
            break;
        case ARG_HEX:
            snprintf(buf, maxlen, "0x%lx", val);
            break;
        case ARG_PTR:
            if (val == 0) snprintf(buf, maxlen, "NULL");
            else snprintf(buf, maxlen, "0x%lx", val);
            break;
        case ARG_INT:
        default:
            snprintf(buf, maxlen, "%ld", (long)val);
            break;
    }
}

/* ── Statistics summary tracker (-c option) ───────────────────────────────── */
typedef struct {
    unsigned long calls;
    unsigned long errors;
} stat_entry_t;

static stat_entry_t g_stats[512];

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: strace [-c] [-p pid] [-o file] command [args...]\n");
        return 1;
    }

    int opt_c = 0;
    pid_t attach_pid = 0;
    const char *out_file = NULL;
    int arg_idx = 1;

    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "-c") == 0) {
            opt_c = 1;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "-p") == 0 && arg_idx + 1 < argc) {
            attach_pid = (pid_t)atoi(argv[arg_idx + 1]);
            arg_idx += 2;
        } else if (strcmp(argv[arg_idx], "-o") == 0 && arg_idx + 1 < argc) {
            out_file = argv[arg_idx + 1];
            arg_idx += 2;
        } else if (strcmp(argv[arg_idx], "--") == 0) {
            arg_idx++;
            break;
        } else {
            fprintf(stderr, "strace: unknown option '%s'\n", argv[arg_idx]);
            return 1;
        }
    }

    FILE *out_fp = stderr;
    if (out_file) {
        out_fp = fopen(out_file, "w");
        if (!out_fp) {
            fprintf(stderr, "strace: cannot open output file '%s': %s\n", out_file, strerror(errno));
            return 1;
        }
    }

    pid_t child = attach_pid;

    if (attach_pid == 0) {
        if (arg_idx >= argc) {
            fprintf(stderr, "strace: must have PROG [ARGS] or -p PID\n");
            if (out_file) fclose(out_fp);
            return 1;
        }

        child = fork();
        if (child < 0) {
            fprintf(stderr, "strace: fork failed: %s\n", strerror(errno));
            if (out_file) fclose(out_fp);
            return 1;
        }

        if (child == 0) {
            /* Tracee child process */
            if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
                perror("ptrace(TRACEME)");
                _exit(1);
            }
            kill(getpid(), SIGSTOP);
            execvp(argv[arg_idx], &argv[arg_idx]);
            fprintf(stderr, "strace: exec '%s' failed: %s\n", argv[arg_idx], strerror(errno));
            _exit(127);
        }
    } else {
        if (ptrace(PTRACE_ATTACH, child, NULL, NULL) < 0) {
            fprintf(stderr, "strace: attach to %d failed: %s\n", child, strerror(errno));
            if (out_file) fclose(out_fp);
            return 1;
        }
    }

    /* Wait for child to be stopped */
    int status;
    while (waitpid(child, &status, WUNTRACED) < 0) {
        if (errno == EINTR) continue;
        break;
    }

    /* Request syscall stop notifications with bit 7 set (SIGTRAP | 0x80) */
    ptrace(PTRACE_SETOPTIONS, child, NULL, (void *)PTRACE_O_TRACESYSGOOD);

    int in_syscall = 0;
    int current_nr = 0;
    int deliver_sig = 0;
    char call_prefix[256];
    call_prefix[0] = '\0';

    for (;;) {
        if (ptrace(PTRACE_SYSCALL, child, NULL, (void *)(long)deliver_sig) < 0) {
            break;
        }
        deliver_sig = 0;

        if (waitpid(child, &status, 0) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (WIFEXITED(status)) {
            if (!opt_c) {
                fprintf(out_fp, "+++ exited with %d +++\n", WEXITSTATUS(status));
            }
            break;
        }

        if (WIFSIGNALED(status)) {
            if (!opt_c) {
                fprintf(out_fp, "+++ killed by %s +++\n", signal_name(WTERMSIG(status)));
            }
            break;
        }

        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);

            /* Check if this is a syscall stop */
            if (sig == (SIGTRAP | 0x80) || sig == SIGTRAP) {
                struct user_regs_struct regs;
                if (ptrace(PTRACE_GETREGS, child, NULL, &regs) < 0) {
                    continue;
                }

                if (!in_syscall) {
                    /* Syscall Entry */
                    in_syscall = 1;
                    current_nr = (int)regs.orig_rax;
                    const syscall_desc_t *desc = find_syscall(current_nr);

                    const char *sname = desc ? desc->name : "unknown_syscall";
                    int nargs = desc ? desc->nargs : 3;

                    unsigned long args[6] = { regs.rdi, regs.rsi, regs.rdx, regs.r10, regs.r8, regs.r9 };

                    char arg_strs[6][MAX_STR_LEN];
                    for (int i = 0; i < nargs && i < 6; i++) {
                        arg_type_t t = desc ? desc->arg_types[i] : ARG_HEX;
                        format_arg(child, t, args[i], arg_strs[i], sizeof(arg_strs[i]));
                    }

                    int pos = snprintf(call_prefix, sizeof(call_prefix), "%s(", sname);
                    for (int i = 0; i < nargs; i++) {
                        pos += snprintf(call_prefix + pos, sizeof(call_prefix) - pos,
                                        "%s%s", arg_strs[i], (i + 1 < nargs) ? ", " : "");
                    }
                    snprintf(call_prefix + pos, sizeof(call_prefix) - pos, ") ");
                } else {
                    /* Syscall Exit */
                    in_syscall = 0;
                    long ret = (long)regs.rax;

                    if (opt_c) {
                        if (current_nr >= 0 && current_nr < 512) {
                            g_stats[current_nr].calls++;
                            if (ret < 0 && ret >= -4095) g_stats[current_nr].errors++;
                        }
                    } else {
                        if (ret < 0 && ret >= -4095) {
                            int err = (int)-ret;
                            fprintf(out_fp, "%-40s = -1 %s\n", call_prefix, errno_name(err));
                        } else if (current_nr == 9 || current_nr == 12) { /* mmap or brk */
                            fprintf(out_fp, "%-40s = 0x%lx\n", call_prefix, (unsigned long)ret);
                        } else {
                            fprintf(out_fp, "%-40s = %ld\n", call_prefix, ret);
                        }
                        fflush(out_fp);
                    }
                }
            } else if (sig != SIGSTOP) {
                /* Delivered Signal — forward to tracee */
                deliver_sig = sig;
                if (!opt_c) {
                    fprintf(out_fp, "--- %s {si_signo=%s} ---\n", signal_name(sig), signal_name(sig));
                    fflush(out_fp);
                }
            }
        }
    }

    /* Print statistics summary if requested */
    if (opt_c) {
        fprintf(out_fp, "%% time     seconds  usecs/call     calls    errors syscall\n");
        fprintf(out_fp, "------ ----------- ----------- --------- --------- ----------------\n");
        unsigned long total_calls = 0;
        unsigned long total_errors = 0;

        for (int i = 0; i < 512; i++) {
            if (g_stats[i].calls > 0) {
                const syscall_desc_t *desc = find_syscall(i);
                const char *name = desc ? desc->name : "unknown";
                fprintf(out_fp, "                  %10lu %9lu %s\n",
                        g_stats[i].calls, g_stats[i].errors, name);
                total_calls += g_stats[i].calls;
                total_errors += g_stats[i].errors;
            }
        }
        fprintf(out_fp, "------ ----------- ----------- --------- --------- ----------------\n");
        fprintf(out_fp, "                  %10lu %9lu total\n", total_calls, total_errors);
    }

    if (out_file) fclose(out_fp);
    return 0;
}
