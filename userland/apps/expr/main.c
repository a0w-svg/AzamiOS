/* ============================================================================
 * AzamiOS Userspace — expr (Evaluate expressions)
 * File: userland/apps/expr/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static int is_number(const char *s)
{
    if (!s || !*s) return 0;
    if (*s == '-' || *s == '+') s++;
    if (!*s) return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "expr: missing operand\n");
        return 2;
    }

    if (argc == 2) {
        printf("%s\n", argv[1]);
        if (strcmp(argv[1], "0") == 0 || argv[1][0] == '\0') return 1;
        return 0;
    }

    if (argc == 4) {
        const char *left = argv[1];
        const char *op = argv[2];
        const char *right = argv[3];

        if (strcmp(op, "+") == 0) {
            long long res = atoll(left) + atoll(right);
            printf("%lld\n", res);
            return res == 0 ? 1 : 0;
        } else if (strcmp(op, "-") == 0) {
            long long res = atoll(left) - atoll(right);
            printf("%lld\n", res);
            return res == 0 ? 1 : 0;
        } else if (strcmp(op, "*") == 0) {
            long long res = atoll(left) * atoll(right);
            printf("%lld\n", res);
            return res == 0 ? 1 : 0;
        } else if (strcmp(op, "/") == 0) {
            long long d = atoll(right);
            if (d == 0) {
                fprintf(stderr, "expr: division by zero\n");
                return 2;
            }
            long long res = atoll(left) / d;
            printf("%lld\n", res);
            return res == 0 ? 1 : 0;
        } else if (strcmp(op, "%") == 0) {
            long long d = atoll(right);
            if (d == 0) {
                fprintf(stderr, "expr: division by zero\n");
                return 2;
            }
            long long res = atoll(left) % d;
            printf("%lld\n", res);
            return res == 0 ? 1 : 0;
        } else if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
            int res;
            if (is_number(left) && is_number(right)) {
                res = (atoll(left) == atoll(right));
            } else {
                res = (strcmp(left, right) == 0);
            }
            printf("%d\n", res);
            return res ? 0 : 1;
        } else if (strcmp(op, "!=") == 0) {
            int res;
            if (is_number(left) && is_number(right)) {
                res = (atoll(left) != atoll(right));
            } else {
                res = (strcmp(left, right) != 0);
            }
            printf("%d\n", res);
            return res ? 0 : 1;
        } else if (strcmp(op, "<") == 0) {
            int res;
            if (is_number(left) && is_number(right)) {
                res = (atoll(left) < atoll(right));
            } else {
                res = (strcmp(left, right) < 0);
            }
            printf("%d\n", res);
            return res ? 0 : 1;
        } else if (strcmp(op, "<=") == 0) {
            int res;
            if (is_number(left) && is_number(right)) {
                res = (atoll(left) <= atoll(right));
            } else {
                res = (strcmp(left, right) <= 0);
            }
            printf("%d\n", res);
            return res ? 0 : 1;
        } else if (strcmp(op, ">") == 0) {
            int res;
            if (is_number(left) && is_number(right)) {
                res = (atoll(left) > atoll(right));
            } else {
                res = (strcmp(left, right) > 0);
            }
            printf("%d\n", res);
            return res ? 0 : 1;
        } else if (strcmp(op, ">=") == 0) {
            int res;
            if (is_number(left) && is_number(right)) {
                res = (atoll(left) >= atoll(right));
            } else {
                res = (strcmp(left, right) >= 0);
            }
            printf("%d\n", res);
            return res ? 0 : 1;
        } else if (strcmp(op, "|") == 0) {
            if (strcmp(left, "0") != 0 && left[0] != '\0') {
                printf("%s\n", left);
                return 0;
            } else if (strcmp(right, "0") != 0 && right[0] != '\0') {
                printf("%s\n", right);
                return 0;
            } else {
                printf("0\n");
                return 1;
            }
        } else if (strcmp(op, "&") == 0) {
            if ((strcmp(left, "0") != 0 && left[0] != '\0') &&
                (strcmp(right, "0") != 0 && right[0] != '\0')) {
                printf("%s\n", left);
                return 0;
            } else {
                printf("0\n");
                return 1;
            }
        }
    }

    fprintf(stderr, "expr: syntax error\n");
    return 2;
}
