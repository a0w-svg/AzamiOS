/* ============================================================================
 * AzamiOS Userspace — Command Execution Timeout Tool (timeout)
 * File: userland/apps/timeout/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static pid_t g_child_pid = 0;

static void alarm_handler(int sig)
{
    (void)sig;
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        usleep(50000);
        kill(g_child_pid, SIGKILL);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("Usage: timeout DURATION COMMAND [ARG]...\n");
        printf("Start COMMAND, and kill it if still running after DURATION seconds.\n");
        return 125;
    }

    int duration = atoi(argv[1]);
    if (duration <= 0) {
        fprintf(stderr, "timeout: invalid duration '%s'\n", argv[1]);
        return 125;
    }

    signal(SIGALRM, alarm_handler);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 125;
    }

    if (pid == 0) {
        execvp(argv[2], &argv[2]);
        perror(argv[2]);
        exit(127);
    }

    g_child_pid = pid;
    alarm((unsigned int)duration);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return 0;
}
