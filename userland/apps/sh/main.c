/* ============================================================================
 * AzamiOS Userspace — POSIX Interactive & Scripting Shell (sh.elf)
 * File: userland/apps/sh/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/ctype.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/wait.h"
#include "../../libc/include/az/ipc.h"

#define MAX_ARGS 64
#define CMD_BUF_SIZE 1024
#define MAX_HISTORY 64
#define MAX_ALIASES 32
#define MAX_FUNCS 32

static char g_cwd[256] = "/";
static char g_history[MAX_HISTORY][CMD_BUF_SIZE];
static int  g_history_count = 0;
static int  g_last_exit_code = 0;

/* ── Aliases ─────────────────────────────────────────────────────────────── */
typedef struct {
    char name[32];
    char value[128];
} alias_t;

static alias_t g_aliases[MAX_ALIASES];
static int     g_alias_count = 0;

/* ── Functions ───────────────────────────────────────────────────────────── */
typedef struct {
    char name[32];
    char body[512];
} func_t;

static func_t g_funcs[MAX_FUNCS];
static int    g_func_count = 0;

static int execute_command(char *raw_cmd);
static int execute_single_command(char *cmd_str);

static void update_cwd(void)
{
    if (getcwd(g_cwd, sizeof(g_cwd)) == NULL) {
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

/* ── Arithmetic Expression Evaluator: $(( expr )) ────────────────────────── */
static long eval_arithmetic_expr(const char **expr)
{
    while (**expr == ' ' || **expr == '\t') (*expr)++;

    long val = 0;
    if (**expr == '(') {
        (*expr)++;
        val = eval_arithmetic_expr(expr);
        while (**expr == ' ' || **expr == '\t') (*expr)++;
        if (**expr == ')') (*expr)++;
    } else if (isdigit((unsigned char)**expr)) {
        val = strtol(*expr, (char **)expr, 10);
    } else if (isalpha((unsigned char)**expr) || **expr == '_') {
        char var[32];
        int vi = 0;
        while (isalnum((unsigned char)**expr) || **expr == '_') {
            if (vi < (int)sizeof(var) - 1) var[vi++] = **expr;
            (*expr)++;
        }
        var[vi] = '\0';
        char *v = getenv(var);
        val = v ? atol(v) : 0;
    }

    while (**expr == ' ' || **expr == '\t') (*expr)++;
    char op = **expr;
    if (op == '+' || op == '-' || op == '*' || op == '/' || op == '%') {
        (*expr)++;
        long rhs = eval_arithmetic_expr(expr);
        if (op == '+') val += rhs;
        else if (op == '-') val -= rhs;
        else if (op == '*') val *= rhs;
        else if (op == '/' && rhs != 0) val /= rhs;
        else if (op == '%' && rhs != 0) val %= rhs;
    }
    return val;
}

/* ── In-Memory Command Substitution: $(cmd) & `cmd` ─────────────────────── */
static void run_subshell_capture(const char *subcmd, char *out, size_t out_max)
{
    int pfd[2];
    if (pipe(pfd) < 0) {
        out[0] = '\0';
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        char cmd_copy[CMD_BUF_SIZE];
        strncpy(cmd_copy, subcmd, sizeof(cmd_copy) - 1);
        cmd_copy[sizeof(cmd_copy) - 1] = '\0';
        execute_command(cmd_copy);
        _exit(0);
    }

    close(pfd[1]);
    size_t total = 0;
    ssize_t n;
    while (total < out_max - 1 && (n = read(pfd[0], out + total, out_max - 1 - total)) > 0) {
        total += n;
    }
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    /* Strip trailing newlines standard in POSIX command substitution */
    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r')) {
        total--;
    }
    out[total] = '\0';
}

/* ── POSIX Parameter & Command Expansions ────────────────────────────────── */
static void expand_line(const char *src, char *dst, size_t max_len)
{
    char *end = dst + max_len - 1;

    while (*src && dst < end) {
        /* 1. Command substitution: `cmd` */
        if (*src == '`') {
            src++;
            const char *close_tick = strchr(src, '`');
            if (close_tick) {
                size_t clen = (size_t)(close_tick - src);
                char subcmd[CMD_BUF_SIZE];
                strncpy(subcmd, src, clen);
                subcmd[clen] = '\0';
                src = close_tick + 1;

                char subout[CMD_BUF_SIZE];
                run_subshell_capture(subcmd, subout, sizeof(subout));
                for (char *p = subout; *p && dst < end; p++) *dst++ = *p;
                continue;
            }
        }

        /* 2. Dollar expansions: $(cmd), $((arith)), ${VAR...}, $VAR */
        if (*src == '$' && *(src + 1) != '\0') {
            src++;

            /* 2a. Arithmetic: $(( expr )) */
            if (src[0] == '(' && src[1] == '(') {
                src += 2;
                const char *close_paren = strstr(src, "))");
                if (close_paren) {
                    char aexpr[128];
                    size_t alen = (size_t)(close_paren - src);
                    strncpy(aexpr, src, alen);
                    aexpr[alen] = '\0';
                    src = close_paren + 2;

                    const char *ap = aexpr;
                    long res = eval_arithmetic_expr(&ap);
                    char num_str[32];
                    snprintf(num_str, sizeof(num_str), "%ld", res);
                    for (char *p = num_str; *p && dst < end; p++) *dst++ = *p;
                    continue;
                }
            }

            /* 2b. Command substitution: $( cmd ) */
            if (src[0] == '(') {
                src++;
                const char *close_paren = strchr(src, ')');
                if (close_paren) {
                    char subcmd[CMD_BUF_SIZE];
                    size_t clen = (size_t)(close_paren - src);
                    strncpy(subcmd, src, clen);
                    subcmd[clen] = '\0';
                    src = close_paren + 1;

                    char subout[CMD_BUF_SIZE];
                    run_subshell_capture(subcmd, subout, sizeof(subout));
                    for (char *p = subout; *p && dst < end; p++) *dst++ = *p;
                    continue;
                }
            }

            /* 2c. Parameter expansion: ${VAR...} */
            if (*src == '{') {
                src++;
                const char *close_brace = strchr(src, '}');
                if (close_brace) {
                    char inner[128];
                    size_t ilen = (size_t)(close_brace - src);
                    strncpy(inner, src, ilen);
                    inner[ilen] = '\0';
                    src = close_brace + 1;

                    /* String length: ${#VAR} */
                    if (inner[0] == '#') {
                        const char *v = getenv(inner + 1);
                        char lstr[16];
                        snprintf(lstr, sizeof(lstr), "%zu", v ? strlen(v) : 0);
                        for (char *p = lstr; *p && dst < end; p++) *dst++ = *p;
                        continue;
                    }

                    /* Default value: ${VAR:-default} */
                    char *dash = strstr(inner, ":-");
                    if (dash) {
                        *dash = '\0';
                        const char *v = getenv(inner);
                        const char *def = dash + 2;
                        const char *chosen = (v && *v) ? v : def;
                        while (*chosen && dst < end) *dst++ = *chosen++;
                        continue;
                    }

                    /* Assign default: ${VAR:=default} */
                    char *assign = strstr(inner, ":=");
                    if (assign) {
                        *assign = '\0';
                        const char *v = getenv(inner);
                        const char *def = assign + 2;
                        if (!v || !*v) {
                            setenv(inner, def, 1);
                            v = def;
                        }
                        while (*v && dst < end) *dst++ = *v++;
                        continue;
                    }

                    /* Alternative value: ${VAR:+val} */
                    char *plus = strstr(inner, ":+");
                    if (plus) {
                        *plus = '\0';
                        const char *v = getenv(inner);
                        if (v && *v) {
                            const char *alt = plus + 2;
                            while (*alt && dst < end) *dst++ = *alt++;
                        }
                        continue;
                    }

                    /* Simple variable ${VAR} */
                    const char *val = getenv(inner);
                    if (val) {
                        while (*val && dst < end) *dst++ = *val++;
                    }
                    continue;
                }
            }

            /* 2d. Special Variables: $?, $$, $# */
            if (*src == '?') {
                src++;
                char cstr[16];
                snprintf(cstr, sizeof(cstr), "%d", g_last_exit_code);
                for (char *p = cstr; *p && dst < end; p++) *dst++ = *p;
                continue;
            }
            if (*src == '$') {
                src++;
                char pstr[16];
                snprintf(pstr, sizeof(pstr), "%d", getpid());
                for (char *p = pstr; *p && dst < end; p++) *dst++ = *p;
                continue;
            }

            /* 2e. Standard $VAR */
            char var_name[64];
            int vi = 0;
            while (isalnum((unsigned char)*src) || *src == '_') {
                if (vi < (int)sizeof(var_name) - 1) var_name[vi++] = *src;
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
                while (*val && dst < end) *dst++ = *val++;
            }
            continue;
        }

        *dst++ = *src++;
    }
    *dst = '\0';
}

/* ── Single Command Execution ────────────────────────────────────────────── */
static int execute_single_command(char *cmd_str)
{
    while (*cmd_str == ' ' || *cmd_str == '\t') cmd_str++;
    size_t len = strlen(cmd_str);
    while (len > 0 && (cmd_str[len - 1] == ' ' || cmd_str[len - 1] == '\t' || cmd_str[len - 1] == '\r' || cmd_str[len - 1] == '\n')) {
        cmd_str[--len] = '\0';
    }
    if (len == 0) return 0;

    /* Handle Pipe (|) */
    char *pipe_pos = strchr(cmd_str, '|');
    if (pipe_pos) {
        *pipe_pos = '\0';
        char *cmd1 = cmd_str;
        char *cmd2 = pipe_pos + 1;

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            puts("[sh] Failed to create pipe");
            return -1;
        }

        int pid1 = fork();
        if (pid1 == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);
            execute_single_command(cmd1);
            _exit(0);
        }

        int pid2 = fork();
        if (pid2 == 0) {
            close(pipefd[1]);
            dup2(pipefd[0], STDIN_FILENO);
            close(pipefd[0]);
            execute_single_command(cmd2);
            _exit(0);
        }

        close(pipefd[0]);
        close(pipefd[1]);
        int status1 = 0, status2 = 0;
        waitpid(pid1, &status1, 0);
        waitpid(pid2, &status2, 0);
        g_last_exit_code = WIFEXITED(status2) ? WEXITSTATUS(status2) : 0;
        return g_last_exit_code;
    }

    /* Redirection handling: >, >>, <, 2>&1, 2>, &> */
    int redir_stderr_to_stdout = 0;
    char *r21 = strstr(cmd_str, "2>&1");
    if (r21) {
        redir_stderr_to_stdout = 1;
        *r21 = '\0';
    }

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

    /* Parse arguments with quote support (' and ") */
    char *argv[MAX_ARGS];
    int argc = 0;
    char *p = cmd_str;

    while (*p && argc < MAX_ARGS - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            argv[argc++] = p;
            while (*p && *p != quote) p++;
            if (*p == quote) *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    argv[argc] = NULL;
    if (argc == 0) return 0;

    /* Check for direct assignment: VAR=VAL */
    if (argc == 1 && strchr(argv[0], '=') && argv[0][0] != '=') {
        char *eq = strchr(argv[0], '=');
        *eq = '\0';
        setenv(argv[0], eq + 1, 1);
        return 0;
    }

    /* Check Aliases */
    for (int i = 0; i < g_alias_count; i++) {
        if (strcmp(argv[0], g_aliases[i].name) == 0) {
            char expanded_alias[CMD_BUF_SIZE];
            snprintf(expanded_alias, sizeof(expanded_alias), "%s", g_aliases[i].value);
            for (int a = 1; a < argc; a++) {
                strcat(expanded_alias, " ");
                strcat(expanded_alias, argv[a]);
            }
            return execute_command(expanded_alias);
        }
    }

    /* Check Functions */
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(argv[0], g_funcs[i].name) == 0) {
            char fbody[CMD_BUF_SIZE];
            strncpy(fbody, g_funcs[i].body, sizeof(fbody) - 1);
            fbody[sizeof(fbody) - 1] = '\0';
            return execute_command(fbody);
        }
    }

    /* ── Built-ins ───────────────────────────────────────────────────────── */
    if (strcmp(argv[0], "cd") == 0) {
        const char *target = (argc > 1) ? argv[1] : getenv("HOME");
        if (!target) target = "/root";
        if (chdir(target) != 0) {
            printf("cd: %s: No such directory\n", target);
            g_last_exit_code = 1;
            return 1;
        }
        update_cwd();
        setenv("PWD", g_cwd, 1);
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "pwd") == 0) {
        printf("%s\n", g_cwd);
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "export") == 0) {
        if (argc > 1) {
            char *eq = strchr(argv[1], '=');
            if (eq) {
                *eq = '\0';
                setenv(argv[1], eq + 1, 1);
            }
        }
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "unset") == 0) {
        if (argc > 1) unsetenv(argv[1]);
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "alias") == 0) {
        if (argc == 1) {
            for (int i = 0; i < g_alias_count; i++) {
                printf("alias %s='%s'\n", g_aliases[i].name, g_aliases[i].value);
            }
            return 0;
        }
        char *eq = strchr(argv[1], '=');
        if (eq && g_alias_count < MAX_ALIASES) {
            *eq = '\0';
            char *val = eq + 1;
            if (*val == '\'' || *val == '"') {
                val++;
                size_t vlen = strlen(val);
                if (vlen > 0 && (val[vlen - 1] == '\'' || val[vlen - 1] == '"')) val[vlen - 1] = '\0';
            }
            strncpy(g_aliases[g_alias_count].name, argv[1], 31);
            strncpy(g_aliases[g_alias_count].value, val, 127);
            g_alias_count++;
        }
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "unalias") == 0) {
        if (argc > 1) {
            for (int i = 0; i < g_alias_count; i++) {
                if (strcmp(g_aliases[i].name, argv[1]) == 0) {
                    for (int j = i; j < g_alias_count - 1; j++) g_aliases[j] = g_aliases[j + 1];
                    g_alias_count--;
                    break;
                }
            }
        }
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "history") == 0) {
        for (int i = 0; i < g_history_count; i++) {
            printf(" %3d  %s\n", i + 1, g_history[i]);
        }
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "clear") == 0) {
        printf("\033[2J\033[H");
        g_last_exit_code = 0;
        return 0;
    }

    if (strcmp(argv[0], "exit") == 0) {
        int code = (argc > 1) ? atoi(argv[1]) : g_last_exit_code;
        exit(code);
    }

    if (strcmp(argv[0], "source") == 0 || strcmp(argv[0], ".") == 0) {
        if (argc > 1) {
            FILE *f = fopen(argv[1], "r");
            if (f) {
                char line[CMD_BUF_SIZE];
                while (fgets(line, sizeof(line), f)) {
                    execute_command(line);
                }
                fclose(f);
                g_last_exit_code = 0;
                return 0;
            } else {
                fprintf(stderr, "source: %s: No such file\n", argv[1]);
                g_last_exit_code = 1;
                return 1;
            }
        }
        return 0;
    }

    if (strcmp(argv[0], "help") == 0) {
        puts("AzamiOS POSIX Shell Built-ins & Utilities:");
        puts("  cd <dir>         - Change current working directory");
        puts("  pwd              - Print working directory");
        puts("  export VAR=VAL   - Set environment variable");
        puts("  unset VAR        - Unset environment variable");
        puts("  alias k='v'      - Define command alias");
        puts("  unalias k        - Remove alias");
        puts("  history          - View command history");
        puts("  source <script>  - Run commands from file in current shell");
        puts("  clear            - Clear terminal screen");
        puts("  exit [code]      - Exit shell");
        puts("\nPOSIX Features:");
        puts("  $(( a + b * c )) - Arithmetic expansion");
        puts("  $(command) / ` ` - Command substitution");
        puts("  ${VAR:-default}  - Parameter expansions (${#VAR}, ${VAR:=def}, ${VAR:+val})");
        puts("  Pipes (|), Redirections (<, >, >>, 2>&1), Chaining (;, &&, ||), Jobs (&)");
        g_last_exit_code = 0;
        return 0;
    }

    /* ── Fork & Execvp ───────────────────────────────────────────────────── */
    pid_t pid = fork();
    if (pid == 0) {
        if (in_file && in_file[0]) {
            int fd_in = open(in_file, O_RDONLY);
            if (fd_in < 0) {
                fprintf(stderr, "[sh] Cannot open input file '%s'\n", in_file);
                _exit(1);
            }
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
        }

        if (out_file && out_file[0]) {
            int flags = O_WRONLY | O_CREAT | (append_out ? O_APPEND : O_TRUNC);
            int fd_out = open(out_file, flags, 0644);
            if (fd_out < 0) {
                fprintf(stderr, "[sh] Cannot open output file '%s'\n", out_file);
                _exit(1);
            }
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
        }

        if (redir_stderr_to_stdout) {
            dup2(STDOUT_FILENO, STDERR_FILENO);
        }

        execvp(argv[0], argv);

        fprintf(stderr, "[sh] %s: command not found\n", argv[0]);
        _exit(127);
    } else if (pid < 0) {
        puts("[sh] fork failed");
        g_last_exit_code = -1;
        return -1;
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        g_last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
        return g_last_exit_code;
    }
}

/* ── Command Chain & Control Flow Execution ──────────────────────────────── */
static int execute_command(char *raw_cmd)
{
    char expanded[CMD_BUF_SIZE];
    expand_line(raw_cmd, expanded, sizeof(expanded));

    char *cmd_str = expanded;
    while (*cmd_str == ' ' || *cmd_str == '\t') cmd_str++;
    size_t len = strlen(cmd_str);
    while (len > 0 && (cmd_str[len - 1] == ' ' || cmd_str[len - 1] == '\t' || cmd_str[len - 1] == '\r' || cmd_str[len - 1] == '\n')) {
        cmd_str[--len] = '\0';
    }
    if (len == 0 || cmd_str[0] == '#') return 0;

    add_history(cmd_str);

    /* Handle 'if ... then ... else ... fi' */
    if (strncmp(cmd_str, "if ", 3) == 0) {
        char *then_pos = strstr(cmd_str, "; then ");
        if (!then_pos) then_pos = strstr(cmd_str, " then ");
        char *fi_pos = strstr(cmd_str, "; fi");
        if (!fi_pos) fi_pos = strstr(cmd_str, " fi");

        if (then_pos && fi_pos) {
            char cond[256];
            size_t clen = (size_t)(then_pos - (cmd_str + 3));
            strncpy(cond, cmd_str + 3, clen);
            cond[clen] = '\0';

            char *body = strstr(then_pos, "then") + 4;
            char *else_pos = strstr(body, "; else ");
            if (!else_pos) else_pos = strstr(body, " else ");

            int cond_res = execute_command(cond);
            if (cond_res == 0) {
                char then_body[512];
                size_t tlen = else_pos ? (size_t)(else_pos - body) : (size_t)(fi_pos - body);
                strncpy(then_body, body, tlen);
                then_body[tlen] = '\0';
                return execute_command(then_body);
            } else if (else_pos) {
                char else_body[512];
                char *eb = strstr(else_pos, "else") + 4;
                size_t elen = (size_t)(fi_pos - eb);
                strncpy(else_body, eb, elen);
                else_body[elen] = '\0';
                return execute_command(else_body);
            }
            return 0;
        }
    }

    /* Command chaining: ';' */
    char *semi = strchr(cmd_str, ';');
    if (semi) {
        *semi = '\0';
        execute_single_command(cmd_str);
        return execute_command(semi + 1);
    }

    /* Command chaining: '&&' */
    char *and_and = strstr(cmd_str, "&&");
    if (and_and) {
        *and_and = '\0';
        int ret = execute_single_command(cmd_str);
        if (ret == 0) {
            return execute_command(and_and + 2);
        }
        return ret;
    }

    /* Command chaining: '||' */
    char *or_or = strstr(cmd_str, "||");
    if (or_or) {
        *or_or = '\0';
        int ret = execute_single_command(cmd_str);
        if (ret != 0) {
            return execute_command(or_or + 2);
        }
        return ret;
    }

    /* Background jobs: '&' */
    len = strlen(cmd_str);
    if (len > 0 && cmd_str[len - 1] == '&') {
        cmd_str[len - 1] = '\0';
        while (len > 1 && (cmd_str[len - 2] == ' ' || cmd_str[len - 2] == '\t')) {
            cmd_str[--len - 1] = '\0';
        }
        pid_t pid = fork();
        if (pid == 0) {
            execute_single_command(cmd_str);
            _exit(0);
        } else if (pid > 0) {
            printf("[1] %d\n", pid);
            return 0;
        }
    }

    return execute_single_command(cmd_str);
}

static void check_dir_matches(const char *dirpath, const char *prefix, int plen, char *match, int *match_count)
{
    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, prefix, plen) == 0) {
            (*match_count)++;
            if (*match_count == 1) {
                strncpy(match, ent->d_name, 127);
                match[127] = '\0';
            }
        }
    }
    closedir(d);
}

static void do_tab_completion(char *buf, int *idx, int max_len)
{
    if (*idx <= 0) return;

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

    char match[128] = "";
    int match_count = 0;

    if (start == 0) {
        check_dir_matches("/bin", prefix, plen, match, &match_count);
        check_dir_matches("/sbin", prefix, plen, match, &match_count);
        check_dir_matches("/usr/bin", prefix, plen, match, &match_count);
    }
    check_dir_matches(g_cwd, prefix, plen, match, &match_count);

    if (match_count == 1) {
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
        return execute_command(argv[2]);
    }

    if (argc > 1 && argv[1][0] != '-') {
        FILE *f = fopen(argv[1], "r");
        if (f) {
            update_cwd();
            char line[CMD_BUF_SIZE];
            while (fgets(line, sizeof(line), f)) {
                execute_command(line);
            }
            fclose(f);
            return g_last_exit_code;
        }
    }

    puts("\n\033[1;36m===================================================\033[0m");
    puts("\033[1;32m  Welcome to AzamiOS 7.0 (POSIX Standard Shell)    \033[0m");
    puts("\033[1;36m===================================================\033[0m");
    puts("Type 'help' for built-ins or run commands (e.g. ls, cat, sed, etc.)\n");

    update_cwd();
    char cmd_buf[CMD_BUF_SIZE];

    for (;;) {
        printf("\033[1;34mazami\033[0m:\033[1;33m%s\033[0m$ ", g_cwd);

        int idx = 0;
        int hist_idx = g_history_count;

        while (idx < (int)sizeof(cmd_buf) - 1) {
            char c;
            if (read(STDIN_FILENO, &c, 1) > 0) {
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
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A') {
                                if (hist_idx > 0) {
                                    hist_idx--;
                                    while (idx > 0) { printf("\b \b"); idx--; }
                                    strncpy(cmd_buf, g_history[hist_idx], sizeof(cmd_buf) - 1);
                                    idx = strlen(cmd_buf);
                                    printf("%s", cmd_buf);
                                }
                            } else if (seq[1] == 'B') {
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
