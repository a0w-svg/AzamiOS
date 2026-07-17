/**
 * python.c — Lightweight Python / MicroPython Subsystem for AzamiOS
 * Supports arithmetic expressions, variable assignments, print statements, and .py scripts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_VARS 64
typedef struct {
    char name[32];
    int  val;
} py_var_t;

static py_var_t s_vars[MAX_VARS];
static int s_num_vars = 0;

static int get_var(const char *name) {
    for (int i = 0; i < s_num_vars; i++) {
        if (strcmp(s_vars[i].name, name) == 0) return s_vars[i].val;
    }
    return 0;
}

static void set_var(const char *name, int val) {
    for (int i = 0; i < s_num_vars; i++) {
        if (strcmp(s_vars[i].name, name) == 0) {
            s_vars[i].val = val;
            return;
        }
    }
    if (s_num_vars < MAX_VARS) {
        strncpy(s_vars[s_num_vars].name, name, 31);
        s_vars[s_num_vars].name[31] = '\0';
        s_vars[s_num_vars].val = val;
        s_num_vars++;
    }
}

static int eval_expr(const char *expr) {
    while (*expr == ' ') expr++;
    if (*expr == '\0') return 0;

    /* Check addition */
    const char *p = strchr(expr, '+');
    if (p) {
        char left[64];
        int len = p - expr; if (len > 63) len = 63;
        memcpy(left, expr, len); left[len] = '\0';
        return eval_expr(left) + eval_expr(p + 1);
    }
    /* Check subtraction */
    p = strchr(expr, '-');
    if (p && p != expr) {
        char left[64];
        int len = p - expr; if (len > 63) len = 63;
        memcpy(left, expr, len); left[len] = '\0';
        return eval_expr(left) - eval_expr(p + 1);
    }
    /* Check multiplication */
    p = strchr(expr, '*');
    if (p) {
        char left[64];
        int len = p - expr; if (len > 63) len = 63;
        memcpy(left, expr, len); left[len] = '\0';
        return eval_expr(left) * eval_expr(p + 1);
    }

    /* Number or Variable */
    if (expr[0] >= '0' && expr[0] <= '9') {
        return atoi(expr);
    }
    return get_var(expr);
}

static void run_line(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    int l = strlen(line);
    while (l > 0 && (line[l-1] == '\r' || line[l-1] == '\n' || line[l-1] == ' ')) {
        line[--l] = '\0';
    }
    if (l == 0 || line[0] == '#') return;

    /* print(...) */
    if (strncmp(line, "print(", 6) == 0 && line[l-1] == ')') {
        line[l-1] = '\0';
        char *arg = line + 6;
        while (*arg == ' ') arg++;
        if (arg[0] == '"' || arg[0] == '\'') {
            int end = strlen(arg) - 1;
            if (arg[end] == '"' || arg[end] == '\'') arg[end] = '\0';
            printf("%s\n", arg + 1);
        } else {
            printf("%d\n", eval_expr(arg));
        }
        return;
    }

    /* Assignment: var = expr */
    char *eq = strchr(line, '=');
    if (eq && *(eq + 1) != '=') {
        *eq = '\0';
        char *vname = line;
        while (*vname == ' ') vname++;
        int vend = strlen(vname) - 1;
        while (vend >= 0 && vname[vend] == ' ') vname[vend--] = '\0';
        int val = eval_expr(eq + 1);
        set_var(vname, val);
        return;
    }

    /* Bare expression */
    printf("%d\n", eval_expr(line));
}

static void run_script(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("python: cannot open script '%s'\n", path);
        return;
    }
    char buf[1024];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        run_line(line);
        if (!nl) break;
        line = nl + 1;
    }
}

int main(int argc, char **argv) {
    if (argc > 1) {
        run_script(argv[1]);
        return 0;
    }
    printf("AzamiOS MicroPython 3.11 (REPL)\nType 'exit()' to quit.\n");
    char line[128];
    for (;;) {
        printf(">>> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        if (strncmp(line, "exit()", 6) == 0 || strncmp(line, "quit()", 6) == 0) break;
        run_line(line);
    }
    return 0;
}
