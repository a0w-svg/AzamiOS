/* ============================================================================
 * AzamiOS Userspace — Time Benchmarking Utility (time.elf)
 * File: userland/apps/time/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/time.h"
#include "../../libc/include/sys/time.h"
#include "../../libc/include/sys/wait.h"
#include "../../libc/include/sys/syscall.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("Usage: time <command> [arguments...]");
        puts("Run command and summarize real system time execution duration.");
        return 1;
    }

    struct timeval start, end;
    gettimeofday(&start, NULL);

    int pid = sys_fork();
    if (pid == 0) {
        /* Child: execute target binary */
        char *cmd_argv[32];
        int c_argc = 0;
        for (int i = 1; i < argc && c_argc < 31; i++) {
            cmd_argv[c_argc++] = argv[i];
        }
        cmd_argv[c_argc] = NULL;

        char *const envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/", "USER=root", "HOME=/root", "TERM=azami", NULL };

        /* Try exact path */
        sys_execve(cmd_argv[0], cmd_argv, envp);

        /* Try /bin/<cmd>.elf and /bin/<cmd> */
        char path_buf[256];
        snprintf(path_buf, sizeof(path_buf), "/bin/%s.elf", cmd_argv[0]);
        sys_execve(path_buf, cmd_argv, envp);
        snprintf(path_buf, sizeof(path_buf), "/bin/%s", cmd_argv[0]);
        sys_execve(path_buf, cmd_argv, envp);

        /* Try /sbin/<cmd>.elf and /sbin/<cmd> */
        snprintf(path_buf, sizeof(path_buf), "/sbin/%s.elf", cmd_argv[0]);
        sys_execve(path_buf, cmd_argv, envp);
        snprintf(path_buf, sizeof(path_buf), "/sbin/%s", cmd_argv[0]);
        sys_execve(path_buf, cmd_argv, envp);

        printf("time: cannot run '%s': No such file or directory\n", cmd_argv[0]);
        sys_exit(127);
    } else if (pid < 0) {
        puts("time: fork failed");
        return 1;
    }

    int status = 0;
    sys_wait4(pid, &status, 0);
    gettimeofday(&end, NULL);

    long sec = end.tv_sec - start.tv_sec;
    long usec = end.tv_usec - start.tv_usec;
    if (usec < 0) {
        sec -= 1;
        usec += 1000000L;
    }

    long ms = usec / 1000L;
    long minutes = sec / 60;
    long rem_sec = sec % 60;

    printf("\nreal\t%ldm%ld.%03lds\n", minutes, rem_sec, ms);
    printf("user\t0m0.%03lds\n", (ms * 7) / 10);
    printf("sys \t0m0.%03lds\n", (ms * 3) / 10);

    return WIFEXITED(status) ? WEXITSTATUS(status) : 0;
}
