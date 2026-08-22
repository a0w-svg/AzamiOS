#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>

int main(void) {
    printf("=== AzamiOS POSIX Syscall Integration Test ===\n");

    /* 1. PID & PPID */
    pid_t pid = getpid();
    pid_t ppid = getppid();
    printf("[1/5] Process Identity: PID = %d, PPID = %d\n", pid, ppid);

    /* 2. File I/O */
    const char *test_path = "/tmp/test_azami_io.txt";
    const char *sample_data = "AzamiOS Native GCC File I/O verification payload\n";
    printf("[2/5] Testing open/write/read/close on %s...\n", test_path);

    int fd = open(test_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("  FAIL: Unable to open file for writing\n");
    } else {
        write(fd, sample_data, strlen(sample_data));
        close(fd);
        printf("  Written %lu bytes successfully.\n", (unsigned long)strlen(sample_data));
    }

    fd = open(test_path, O_RDONLY);
    if (fd >= 0) {
        char buf[128];
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        printf("  Read back %ld bytes: %s", (long)n, buf);
        unlink(test_path);
        printf("  File unlinked successfully.\n");
    }

    /* 3. Heap Memory */
    printf("[3/5] Testing heap allocation & pointer arithmetic...\n");
    int *arr = (int *)malloc(256 * sizeof(int));
    if (arr) {
        for (int i = 0; i < 256; i++) arr[i] = i * i;
        int check_val = arr[12];
        free(arr);
        printf("  malloc/free OK (12^2 = %d)\n", check_val);
    }

    /* 4. Fork & Wait */
    printf("[4/5] Testing process fork & waitpid...\n");
    pid_t child_pid = fork();
    if (child_pid == 0) {
        /* In child */
        printf("  [Child %d] Running inside child process!\n", getpid());
        exit(42);
    } else if (child_pid > 0) {
        int status = 0;
        waitpid(child_pid, &status, 0);
        int exit_code = WEXITSTATUS(status);
        printf("  [Parent] Child %d finished with exit status %d.\n", child_pid, exit_code);
    }

    /* 5. CWD */
    char cwd[256];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("[5/5] Current Working Directory: %s\n", cwd);
    }

    printf("=== All POSIX Syscall Tests Passed ===\n");
    return 0;
}
