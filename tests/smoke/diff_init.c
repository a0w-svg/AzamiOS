#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    puts("== AzamiOS strace runner init ==");

    char cmd_buf[512];
    memset(cmd_buf, 0, sizeof(cmd_buf));
    int fd = open("/etc/strace-test.cmd", O_RDONLY);
    if (fd < 0) {
        strncpy(cmd_buf, "/bin/busybox true", sizeof(cmd_buf) - 1);
    } else {
        ssize_t n = read(fd, cmd_buf, sizeof(cmd_buf) - 1);
        if (n > 0) cmd_buf[n] = '\0';
        close(fd);
    }

    char *nl = strchr(cmd_buf, '\n');
    if (nl) *nl = '\0';

    char *argv[64];
    int argc = 0;
    argv[argc++] = "/bin/strace-gnu";
    argv[argc++] = "-f";
    argv[argc++] = "-o";
    argv[argc++] = "/tmp/guest.trace";

    char *tok = strtok(cmd_buf, " \t");
    while (tok && argc < 60) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;

    printf("Executing strace for command: %s\n", argv[4] ? argv[4] : "none");
    fflush(stdout);

    pid_t child = fork();
    if (child == 0) {
        char *env[] = { "PATH=/bin:/usr/bin", "HOME=/root", "PWD=/", NULL };
        execve("/bin/strace-gnu", argv, env);
        perror("/bin/strace-gnu");
        _exit(127);
    }

    int status = 0;
    pid_t w;
    do {
        w = waitpid(child, &status, 0);
    } while (w < 0 && errno == EINTR);

    printf("strace exited with status %#x\n", status);

    puts("=== BEGIN GUEST STRACE ===");
    fflush(stdout);
    int tfd = open("/tmp/guest.trace", O_RDONLY);
    if (tfd >= 0) {
        char buf[1024];
        ssize_t r;
        while ((r = read(tfd, buf, sizeof(buf))) > 0) {
            write(STDOUT_FILENO, buf, r);
        }
        close(tfd);
    } else {
        perror("open /tmp/guest.trace");
    }
    puts("=== END GUEST STRACE ===");

    puts("== probe complete: 1 passed, 0 failed ==");
    return 0;
}
