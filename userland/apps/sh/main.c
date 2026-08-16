/* ============================================================================
 * AzamiOS Userspace — Interactive Command Shell (sh.elf)
 * File: userland/apps/sh/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/az/ipc.h"

#define MAX_ARGS 32
#define CMD_BUF_SIZE 512
#define MAX_HISTORY 32

static char g_cwd[256] = "/";
static char g_history[MAX_HISTORY][CMD_BUF_SIZE];
static int  g_history_count = 0;

static void update_cwd(void)
{
    if (sys_getcwd(g_cwd, sizeof(g_cwd)) <= 0) {
        strcpy(g_cwd, "/");
    }
}

static void add_history(const char *cmd)
{
    if (!cmd || !*cmd) return;
    if (g_history_count > 0 && strcmp(g_history[g_history_count - 1], cmd) == 0) return;

    if (g_history_count < MAX_HISTORY) {
        strncpy(g_history[g_history_count++], cmd, CMD_BUF_SIZE - 1);
    } else {
        for (int i = 1; i < MAX_HISTORY; i++) {
            strcpy(g_history[i - 1], g_history[i]);
        }
        strncpy(g_history[MAX_HISTORY - 1], cmd, CMD_BUF_SIZE - 1);
    }
}

static void expand_env_vars(char *cmd_str, char *out_str, size_t max_len)
{
    char *src = cmd_str;
    char *dst = out_str;
    char *end = out_str + max_len - 1;

    while (*src && dst < end) {
        if (*src == '$' && *(src + 1) != '\0') {
            src++;
            char var_name[64];
            int vi = 0;
            while ((*src >= 'A' && *src <= 'Z') || (*src >= 'a' && *src <= 'z') ||
                   (*src >= '0' && *src <= '9') || *src == '_') {
                if (vi < (int)sizeof(var_name) - 1) {
                    var_name[vi++] = *src;
                }
                src++;
            }
            var_name[vi] = '\0';

            const char *val = NULL;
            if (strcmp(var_name, "PWD") == 0 || strcmp(var_name, "CWD") == 0) {
                val = g_cwd;
            } else {
                val = getenv(var_name);
            }

            if (val) {
                while (*val && dst < end) {
                    *dst++ = *val++;
                }
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void execute_command(char *raw_cmd);

static void execute_command(char *raw_cmd)
{
    char expanded[CMD_BUF_SIZE];
    expand_env_vars(raw_cmd, expanded, sizeof(expanded));

    char *cmd_str = expanded;

    /* Trim leading and trailing whitespace */
    while (*cmd_str == ' ' || *cmd_str == '\t') cmd_str++;
    size_t len = strlen(cmd_str);
    while (len > 0 && (cmd_str[len - 1] == ' ' || cmd_str[len - 1] == '\t' || cmd_str[len - 1] == '\r' || cmd_str[len - 1] == '\n')) {
        cmd_str[--len] = '\0';
    }
    if (len == 0) return;

    add_history(cmd_str);

    /* Handle Pipe (|) */
    char *pipe_pos = strchr(cmd_str, '|');
    if (pipe_pos) {
        *pipe_pos = '\0';
        char *cmd1 = cmd_str;
        char *cmd2 = pipe_pos + 1;

        int pipefd[2];
        if (sys_pipe(pipefd) < 0) {
            puts("[sh] Failed to create pipe");
            return;
        }

        int pid1 = sys_fork();
        if (pid1 == 0) {
            sys_close(pipefd[0]);
            if (pipefd[1] != 1) {
                sys_dup2(pipefd[1], 1);
                sys_close(pipefd[1]);
            }
            execute_command(cmd1);
            sys_exit(0);
        }

        int pid2 = sys_fork();
        if (pid2 == 0) {
            sys_close(pipefd[1]);
            if (pipefd[0] != 0) {
                sys_dup2(pipefd[0], 0);
                sys_close(pipefd[0]);
            }
            execute_command(cmd2);
            sys_exit(0);
        }

        sys_close(pipefd[0]);
        sys_close(pipefd[1]);
        int status = 0;
        sys_wait4(pid1, &status, 0);
        sys_wait4(pid2, &status, 0);
        return;
    }

    /* Handle Redirection (> and >> and <) */
    char *redir_out = strchr(cmd_str, '>');
    char *redir_in = strchr(cmd_str, '<');
    int append_out = 0;
    char *out_file = NULL;
    char *in_file = NULL;

    if (redir_out) {
        if (*(redir_out + 1) == '>') {
            append_out = 1;
            *redir_out = '\0';
            out_file = redir_out + 2;
        } else {
            *redir_out = '\0';
            out_file = redir_out + 1;
        }
        while (*out_file == ' ' || *out_file == '\t') out_file++;
        char *end = out_file;
        while (*end && *end != ' ' && *end != '\t' && *end != '<') end++;
        *end = '\0';
    }

    if (redir_in) {
        *redir_in = '\0';
        in_file = redir_in + 1;
        while (*in_file == ' ' || *in_file == '\t') in_file++;
        char *end = in_file;
        while (*end && *end != ' ' && *end != '\t') end++;
        *end = '\0';
    }

    /* Parse arguments */
    char *argv[MAX_ARGS];
    int argc = 0;
    char *p = cmd_str;

    while (*p && argc < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    argv[argc] = NULL;
    if (argc == 0) return;

    /* Built-ins */
    if (strcmp(argv[0], "cd") == 0) {
        const char *target = (argc > 1) ? argv[1] : "/";
        if (sys_chdir(target) < 0) {
            printf("cd: %s: No such directory\n", target);
        } else {
            update_cwd();
        }
        return;
    }

    if (strcmp(argv[0], "pwd") == 0) {
        update_cwd();
        puts(g_cwd);
        return;
    }

    if (strcmp(argv[0], "export") == 0 || strcmp(argv[0], "setenv") == 0) {
        if (argc > 1) {
            char *eq = strchr(argv[1], '=');
            if (eq) {
                *eq = '\0';
                setenv(argv[1], eq + 1, 1);
            } else if (argc > 2) {
                setenv(argv[1], argv[2], 1);
            }
        }
        return;
    }

    if (strcmp(argv[0], "unsetenv") == 0 || strcmp(argv[0], "unset") == 0) {
        if (argc > 1) unsetenv(argv[1]);
        return;
    }

    if (strcmp(argv[0], "history") == 0) {
        for (int i = 0; i < g_history_count; i++) {
            printf("%4d  %s\n", i + 1, g_history[i]);
        }
        return;
    }

    if (strcmp(argv[0], "clear") == 0) {
        printf("\033[2J\033[H");
        return;
    }

    if (strcmp(argv[0], "exit") == 0) {
        int code = (argc > 1) ? atoi(argv[1]) : 0;
        sys_exit(code);
    }

    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        if (argc > 1) {
            int fd = sys_open(argv[1], 0, 0);
            if (fd >= 0) {
                char line[CMD_BUF_SIZE];
                int l_idx = 0;
                char c;
                while (sys_read(fd, &c, 1) > 0) {
                    if (c == '\n' || c == '\r') {
                        line[l_idx] = '\0';
                        execute_command(line);
                        l_idx = 0;
                    } else if (l_idx < (int)sizeof(line) - 1) {
                        line[l_idx++] = c;
                    }
                }
                if (l_idx > 0) {
                    line[l_idx] = '\0';
                    execute_command(line);
                }
                sys_close(fd);
            } else {
                printf("source: %s: No such file\n", argv[1]);
            }
        }
        return;
    }

    if (strcmp(argv[0], "help") == 0) {
        puts("AzamiOS POSIX Shell Built-ins:");
        puts("  cd <dir>         - Change current working directory");
        puts("  pwd              - Print working directory");
        puts("  export VAR=VAL   - Set environment variable");
        puts("  unset VAR        - Unset environment variable");
        puts("  history          - View command history");
        puts("  source <script>  - Run commands from file in current shell");
        puts("  clear            - Clear terminal screen");
        puts("  exit [code]      - Exit shell");
        puts("  help             - Show this help summary");
        puts("\nFeatures:");
        puts("  Pipes (|), Redirections (<, >, >>), $VAR expansion, Tab completion, History (Up/Down)");
        return;
    }

    /* Fork and Execve */
    int pid = sys_fork();
    if (pid == 0) {
        /* Child: Setup Redirections */
        if (in_file && in_file[0]) {
            int fd_in = sys_open(in_file, 0 /* O_RDONLY */, 0);
            if (fd_in < 0) {
                printf("[sh] Cannot open input file '%s'\n", in_file);
                sys_exit(1);
            }
            sys_dup2(fd_in, 0);
            sys_close(fd_in);
        }

        if (out_file && out_file[0]) {
            int flags = 1 /* O_WRONLY */ | 0100 /* O_CREAT */;
            flags |= (append_out ? 02000 /* O_APPEND */ : 01000 /* O_TRUNC */);
            int fd_out = sys_open(out_file, flags, 0644);
            if (fd_out < 0) {
                printf("[sh] Cannot open output file '%s'\n", out_file);
                sys_exit(1);
            }
            sys_dup2(fd_out, 1);
            sys_close(fd_out);
        }

        char *const envp[] = { "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/", "USER=root", "HOME=/root", "TERM=azami", NULL };

        /* 1. Try exact path (e.g. /bin/ls.elf, ./prog) */
        sys_execve(argv[0], argv, envp);

        /* 2. Try /bin/<cmd>.elf and /bin/<cmd> */
        char path_buf[256];
        snprintf(path_buf, sizeof(path_buf), "/bin/%s.elf", argv[0]);
        sys_execve(path_buf, argv, envp);
        snprintf(path_buf, sizeof(path_buf), "/bin/%s", argv[0]);
        sys_execve(path_buf, argv, envp);

        /* 3. Try /sbin/<cmd>.elf and /sbin/<cmd> */
        snprintf(path_buf, sizeof(path_buf), "/sbin/%s.elf", argv[0]);
        sys_execve(path_buf, argv, envp);
        snprintf(path_buf, sizeof(path_buf), "/sbin/%s", argv[0]);
        sys_execve(path_buf, argv, envp);

        /* 4. Try /usr/bin/<cmd>.elf and /usr/bin/<cmd> */
        snprintf(path_buf, sizeof(path_buf), "/usr/bin/%s.elf", argv[0]);
        sys_execve(path_buf, argv, envp);
        snprintf(path_buf, sizeof(path_buf), "/usr/bin/%s", argv[0]);
        sys_execve(path_buf, argv, envp);

        /* 5. Legacy root path fallback */
        snprintf(path_buf, sizeof(path_buf), "/%s.elf", argv[0]);
        sys_execve(path_buf, argv, envp);
        snprintf(path_buf, sizeof(path_buf), "/%s", argv[0]);
        sys_execve(path_buf, argv, envp);

        printf("[sh] %s: command not found\n", argv[0]);
        sys_exit(127);
    } else if (pid < 0) {
        puts("[sh] fork failed");
    } else {
        /* Parent: Wait for child process */
        int status = 0;
        sys_wait4(pid, &status, 0);
    }
}

static void do_tab_completion(char *buf, int *idx, int max_len)
{
    if (*idx <= 0) return;

    /* Find start of current word */
    int start = *idx - 1;
    while (start >= 0 && buf[start] != ' ' && buf[start] != '\t') {
        start--;
    }
    start++;

    char prefix[128];
    int plen = *idx - start;
    if (plen >= (int)sizeof(prefix)) return;
    memcpy(prefix, &buf[start], plen);
    prefix[plen] = '\0';

    DIR *d = opendir(g_cwd);
    if (!d) d = opendir("/");
    if (!d) return;

    char match[128] = "";
    int match_count = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, plen) == 0) {
            match_count++;
            if (match_count == 1) {
                strncpy(match, ent->d_name, sizeof(match) - 1);
            }
        }
    }
    closedir(d);

    if (match_count == 1) {
        /* Auto-complete the remainder */
        for (int i = plen; match[i] != '\0' && *idx < max_len - 1; i++) {
            putchar(match[i]);
            buf[(*idx)++] = match[i];
        }
    }
}

int main(int argc, char **argv)
{
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        update_cwd();
        execute_command(argv[2]);
        return 0;
    }

    if (argc > 1 && argv[1][0] != '-') {
        int fd = sys_open(argv[1], 0, 0);
        if (fd >= 0) {
            char line[CMD_BUF_SIZE];
            int l_idx = 0;
            char c;
            while (sys_read(fd, &c, 1) > 0) {
                if (c == '\n' || c == '\r') {
                    line[l_idx] = '\0';
                    execute_command(line);
                    l_idx = 0;
                } else if (l_idx < (int)sizeof(line) - 1) {
                    line[l_idx++] = c;
                }
            }
            if (l_idx > 0) {
                line[l_idx] = '\0';
                execute_command(line);
            }
            sys_close(fd);
            return 0;
        }
    }

    puts("\n\033[1;36m===================================================\033[0m");
    puts("\033[1;32m  Welcome to AzamiOS 7.0 (SMP POSIX Shell)         \033[0m");
    puts("\033[1;36m===================================================\033[0m");
    puts("Type 'help' for built-ins or run commands (e.g. ls, cat, ping, etc.)\n");

    update_cwd();
    char cmd_buf[CMD_BUF_SIZE];

    for (;;) {
        printf("\033[1;34mazami\033[0m:\033[1;33m%s\033[0m$ ", g_cwd);

        int idx = 0;
        int hist_idx = g_history_count;

        while (idx < (int)sizeof(cmd_buf) - 1) {
            char c;
            if (sys_read(0, &c, 1) > 0) {
                if (c == '\n' || c == '\r') {
                    putchar('\n');
                    break;
                } else if (c == '\b' || c == 0x7F) {
                    if (idx > 0) {
                        idx--;
                        printf("\b \b");
                    }
                } else if (c == '\t') {
                    do_tab_completion(cmd_buf, &idx, sizeof(cmd_buf));
                } else if (c == 0x1B) {
                    /* Escape sequence (e.g. ANSI arrow keys \033[A, \033[B) */
                    char seq[2];
                    if (sys_read(0, &seq[0], 1) > 0 && sys_read(0, &seq[1], 1) > 0) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') {
                                /* Up arrow: previous history */
                                if (hist_idx > 0) {
                                    hist_idx--;
                                    while (idx > 0) { printf("\b \b"); idx--; }
                                    strncpy(cmd_buf, g_history[hist_idx], sizeof(cmd_buf) - 1);
                                    idx = strlen(cmd_buf);
                                    printf("%s", cmd_buf);
                                }
                            } else if (seq[1] == 'B') {
                                /* Down arrow: next history */
                                if (hist_idx < g_history_count - 1) {
                                    hist_idx++;
                                    while (idx > 0) { printf("\b \b"); idx--; }
                                    strncpy(cmd_buf, g_history[hist_idx], sizeof(cmd_buf) - 1);
                                    idx = strlen(cmd_buf);
                                    printf("%s", cmd_buf);
                                } else {
                                    hist_idx = g_history_count;
                                    while (idx > 0) { printf("\b \b"); idx--; }
                                    cmd_buf[0] = '\0';
                                    idx = 0;
                                }
                            }
                        }
                    }
                } else {
                    putchar(c);
                    cmd_buf[idx++] = c;
                }
            } else {
                az_yield();
            }
        }
        cmd_buf[idx] = '\0';
        execute_command(cmd_buf);
    }
    return 0;
}
