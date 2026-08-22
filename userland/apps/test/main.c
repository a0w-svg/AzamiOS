/* ============================================================================
 * AzamiOS Userspace — POSIX Conditional Evaluator (test / [)
 * File: userland/apps/test/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/stat.h"

static int eval_expr(int argc, char **argv, int *pos);

static int eval_primary(int argc, char **argv, int *pos)
{
    if (*pos >= argc) return 1;

    char *arg = argv[*pos];

    /* Logical NOT */
    if (strcmp(arg, "!") == 0) {
        (*pos)++;
        return !eval_primary(argc, argv, pos);
    }

    /* Subexpression in parentheses */
    if (strcmp(arg, "(") == 0) {
        (*pos)++;
        int res = eval_expr(argc, argv, pos);
        if (*pos < argc && strcmp(argv[*pos], ")") == 0) {
            (*pos)++;
        }
        return res;
    }

    /* Unary tests (e.g. -f file, -z str, -n str) */
    if (arg[0] == '-' && arg[1] != '\0' && arg[2] == '\0' && (*pos + 1 < argc)) {
        char op = arg[1];
        char *target = argv[*pos + 1];
        *pos += 2;

        struct stat st;
        int stat_res = stat(target, &st);

        switch (op) {
            case 'e': case 'a': /* File exists */
                return (stat_res == 0) ? 0 : 1;
            case 'f': /* Regular file */
                return (stat_res == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
            case 'd': /* Directory */
                return (stat_res == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
            case 'c': /* Character device */
                return (stat_res == 0 && S_ISCHR(st.st_mode)) ? 0 : 1;
            case 'b': /* Block device */
                return (stat_res == 0 && S_ISBLK(st.st_mode)) ? 0 : 1;
            case 'L': case 'h': /* Symbolic link */
                return (lstat(target, &st) == 0 && S_ISLNK(st.st_mode)) ? 0 : 1;
            case 's': /* Size > 0 */
                return (stat_res == 0 && st.st_size > 0) ? 0 : 1;
            case 'r': /* Readable */
                return (access(target, R_OK) == 0) ? 0 : 1;
            case 'w': /* Writable */
                return (access(target, W_OK) == 0) ? 0 : 1;
            case 'x': /* Executable */
                return (access(target, X_OK) == 0) ? 0 : 1;
            case 'z': /* String length == 0 */
                return (strlen(target) == 0) ? 0 : 1;
            case 'n': /* String length > 0 */
                return (strlen(target) > 0) ? 0 : 1;
            default:
                break;
        }
        /* Fallback if unrecognized unary */
        *pos -= 1;
    }

    /* Binary comparisons (str1 = str2, num1 -eq num2, etc.) */
    if (*pos + 2 < argc) {
        char *s1 = argv[*pos];
        char *op = argv[*pos + 1];
        char *s2 = argv[*pos + 2];

        if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            *pos += 3;
            return (strcmp(s1, s2) == 0) ? 0 : 1;
        }
        if (strcmp(op, "!=") == 0) {
            *pos += 3;
            return (strcmp(s1, s2) != 0) ? 0 : 1;
        }
        if (strcmp(op, "-eq") == 0) {
            *pos += 3;
            return (atol(s1) == atol(s2)) ? 0 : 1;
        }
        if (strcmp(op, "-ne") == 0) {
            *pos += 3;
            return (atol(s1) != atol(s2)) ? 0 : 1;
        }
        if (strcmp(op, "-lt") == 0) {
            *pos += 3;
            return (atol(s1) < atol(s2)) ? 0 : 1;
        }
        if (strcmp(op, "-le") == 0) {
            *pos += 3;
            return (atol(s1) <= atol(s2)) ? 0 : 1;
        }
        if (strcmp(op, "-gt") == 0) {
            *pos += 3;
            return (atol(s1) > atol(s2)) ? 0 : 1;
        }
        if (strcmp(op, "-ge") == 0) {
            *pos += 3;
            return (atol(s1) >= atol(s2)) ? 0 : 1;
        }
    }

    /* Single string test (true if not empty) */
    (*pos)++;
    return (strlen(arg) > 0) ? 0 : 1;
}

static int eval_and(int argc, char **argv, int *pos)
{
    int res = eval_primary(argc, argv, pos);
    while (*pos < argc && strcmp(argv[*pos], "-a") == 0) {
        (*pos)++;
        int next_res = eval_primary(argc, argv, pos);
        res = (res == 0 && next_res == 0) ? 0 : 1;
    }
    return res;
}

static int eval_expr(int argc, char **argv, int *pos)
{
    int res = eval_and(argc, argv, pos);
    while (*pos < argc && strcmp(argv[*pos], "-o") == 0) {
        (*pos)++;
        int next_res = eval_and(argc, argv, pos);
        res = (res == 0 || next_res == 0) ? 0 : 1;
    }
    return res;
}

int main(int argc, char **argv)
{
    /* Check if invoked as '[' (requires closing ']') */
    char *prog = argv[0];
    char *slash = strrchr(prog, '/');
    if (slash) prog = slash + 1;

    int is_bracket = (strcmp(prog, "[") == 0 || strcmp(prog, "[.elf") == 0);
    if (is_bracket) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "[: missing ']'\n");
            return 2;
        }
        argc--; /* Strip trailing ']' */
    }

    if (argc <= 1) {
        return 1; /* Empty expression is false */
    }

    int pos = 1;
    int ret = eval_expr(argc, argv, &pos);
    return ret;
}
