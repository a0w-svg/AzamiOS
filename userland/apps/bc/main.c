/* ============================================================================
 * AzamiOS — Arbitrary Precision & Math Expression Calculator (bc.elf v2.0)
 * File: userland/apps/bc/main.c
 *
 * Supports mathematical expressions, operator precedence (+, -, *, /, %, ^),
 * bitwise operations (&, |, ~, <<, >>), hex/binary/octal radix literals,
 * output base conversion (obase=2, 8, 10, 16), extended math functions
 * (sqrt, sin, cos, abs, log, exp, ceil, floor, round, pow, hypot), and scale.
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/ctype.h"
#include "../../libc/include/math.h"
#include "../../libc/include/unistd.h"

#define MAX_VARS 64
typedef struct {
    char   name[32];
    double value;
} var_t;

static var_t g_vars[MAX_VARS];
static int   g_num_vars = 0;
static int   g_scale = 4;
static int   g_obase = 10;
static double g_last = 0.0;

static double get_var(const char *name)
{
    if (strcmp(name, "scale") == 0) return (double)g_scale;
    if (strcmp(name, "obase") == 0) return (double)g_obase;
    if (strcmp(name, "last") == 0) return g_last;
    if (strcmp(name, "pi") == 0) return 3.141592653589793;
    if (strcmp(name, "e") == 0) return 2.718281828459045;

    for (int i = 0; i < g_num_vars; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            return g_vars[i].value;
        }
    }
    return 0.0;
}

static void set_var(const char *name, double val)
{
    if (strcmp(name, "scale") == 0) {
        g_scale = (int)val;
        if (g_scale < 0) g_scale = 0;
        if (g_scale > 15) g_scale = 15;
        return;
    }
    if (strcmp(name, "obase") == 0) {
        g_obase = (int)val;
        if (g_obase != 2 && g_obase != 8 && g_obase != 10 && g_obase != 16) {
            g_obase = 10;
        }
        return;
    }

    for (int i = 0; i < g_num_vars; i++) {
        if (strcmp(g_vars[i].name, name) == 0) {
            g_vars[i].value = val;
            return;
        }
    }

    if (g_num_vars < MAX_VARS) {
        strncpy(g_vars[g_num_vars].name, name, sizeof(g_vars[0].name) - 1);
        g_vars[g_num_vars].value = val;
        g_num_vars++;
    }
}

/* ── Parser & Evaluator ───────────────────────────────────────────────────── */
typedef struct {
    const char *src;
    size_t      pos;
} parser_t;

static void skip_whitespace(parser_t *p)
{
    while (p->src[p->pos] == ' ' || p->src[p->pos] == '\t') p->pos++;
}

static double parse_expr(parser_t *p);

static double parse_number(parser_t *p)
{
    skip_whitespace(p);
    const char *s = p->src + p->pos;

    /* Hex literal 0x... */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *end;
        unsigned long long val = strtoull(s, &end, 16);
        p->pos += (size_t)(end - s);
        return (double)val;
    }

    /* Binary literal 0b... */
    if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        char *end;
        unsigned long long val = strtoull(s + 2, &end, 2);
        p->pos += (size_t)(end - s);
        return (double)val;
    }

    /* Octal literal 0o... */
    if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
        char *end;
        unsigned long long val = strtoull(s + 2, &end, 8);
        p->pos += (size_t)(end - s);
        return (double)val;
    }

    char *end;
    double val = strtod(s, &end);
    p->pos += (size_t)(end - s);
    return val;
}

static double parse_primary(parser_t *p)
{
    skip_whitespace(p);
    char c = p->src[p->pos];

    if (c == '(') {
        p->pos++;
        double val = parse_expr(p);
        skip_whitespace(p);
        if (p->src[p->pos] == ')') p->pos++;
        return val;
    }

    if (c == '-') {
        p->pos++;
        return -parse_primary(p);
    }

    if (c == '+') {
        p->pos++;
        return parse_primary(p);
    }

    if (c == '~') {
        p->pos++;
        long v = (long)parse_primary(p);
        return (double)(~v);
    }

    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p->src[p->pos + 1]))) {
        return parse_number(p);
    }

    if (isalpha((unsigned char)c) || c == '_') {
        char id[32];
        size_t id_len = 0;
        while (isalnum((unsigned char)p->src[p->pos]) || p->src[p->pos] == '_') {
            if (id_len < sizeof(id) - 1) {
                id[id_len++] = p->src[p->pos];
            }
            p->pos++;
        }
        id[id_len] = '\0';
        skip_whitespace(p);

        /* Function call */
        if (p->src[p->pos] == '(') {
            p->pos++;
            double arg1 = parse_expr(p);
            skip_whitespace(p);
            double arg2 = 0.0;
            if (p->src[p->pos] == ',') {
                p->pos++;
                arg2 = parse_expr(p);
                skip_whitespace(p);
            }
            if (p->src[p->pos] == ')') p->pos++;

            if (strcmp(id, "sqrt") == 0) return sqrt(arg1);
            if (strcmp(id, "sin") == 0) return sin(arg1);
            if (strcmp(id, "cos") == 0) return cos(arg1);
            if (strcmp(id, "tan") == 0) return tan(arg1);
            if (strcmp(id, "abs") == 0) return fabs(arg1);
            if (strcmp(id, "log") == 0) return log(arg1);
            if (strcmp(id, "exp") == 0) return exp(arg1);
            if (strcmp(id, "ceil") == 0) return ceil(arg1);
            if (strcmp(id, "floor") == 0) return floor(arg1);
            if (strcmp(id, "round") == 0) return round(arg1);
            if (strcmp(id, "pow") == 0) return pow(arg1, arg2);
            if (strcmp(id, "hypot") == 0) return hypot(arg1, arg2);
            if (strcmp(id, "atan2") == 0) return atan2(arg1, arg2);

            /* Bitwise functions */
            if (strcmp(id, "band") == 0) return (double)((long)arg1 & (long)arg2);
            if (strcmp(id, "bor") == 0) return (double)((long)arg1 | (long)arg2);
            if (strcmp(id, "bxor") == 0) return (double)((long)arg1 ^ (long)arg2);
            if (strcmp(id, "shl") == 0) return (double)((long)arg1 << (long)arg2);
            if (strcmp(id, "shr") == 0) return (double)((long)arg1 >> (long)arg2);

            return 0.0;
        }

        /* Assignment: var = expr */
        if (p->src[p->pos] == '=') {
            p->pos++;
            double val = parse_expr(p);
            set_var(id, val);
            return val;
        }

        return get_var(id);
    }

    return 0.0;
}

static double parse_power(parser_t *p)
{
    double base = parse_primary(p);
    skip_whitespace(p);
    if (p->src[p->pos] == '^') {
        p->pos++;
        double exp_val = parse_power(p);
        return pow(base, exp_val);
    }
    return base;
}

static double parse_factor(parser_t *p)
{
    double left = parse_power(p);
    while (1) {
        skip_whitespace(p);
        char c = p->src[p->pos];
        if (c == '*' && p->src[p->pos + 1] != '*') {
            p->pos++;
            left *= parse_power(p);
        } else if (c == '/') {
            p->pos++;
            double right = parse_power(p);
            left = (right != 0.0) ? (left / right) : 0.0;
        } else if (c == '%') {
            p->pos++;
            double right = parse_power(p);
            left = (right != 0.0) ? fmod(left, right) : 0.0;
        } else {
            break;
        }
    }
    return left;
}

static double parse_shift(parser_t *p)
{
    double left = parse_factor(p);
    while (1) {
        skip_whitespace(p);
        if (p->src[p->pos] == '<' && p->src[p->pos + 1] == '<') {
            p->pos += 2;
            long r = (long)parse_factor(p);
            left = (double)((long)left << r);
        } else if (p->src[p->pos] == '>' && p->src[p->pos + 1] == '>') {
            p->pos += 2;
            long r = (long)parse_factor(p);
            left = (double)((long)left >> r);
        } else {
            break;
        }
    }
    return left;
}

static double parse_term(parser_t *p)
{
    double left = parse_shift(p);
    while (1) {
        skip_whitespace(p);
        char c = p->src[p->pos];
        if (c == '+') {
            p->pos++;
            left += parse_shift(p);
        } else if (c == '-') {
            p->pos++;
            left -= parse_shift(p);
        } else {
            break;
        }
    }
    return left;
}

static double parse_expr(parser_t *p)
{
    double left = parse_term(p);
    while (1) {
        skip_whitespace(p);
        char c = p->src[p->pos];
        if (c == '&') {
            p->pos++;
            left = (double)((long)left & (long)parse_term(p));
        } else if (c == '|') {
            p->pos++;
            left = (double)((long)left | (long)parse_term(p));
        } else {
            break;
        }
    }
    return left;
}

/* ── Result Formatting (Scale & Radix) ────────────────────────────────────── */
static void print_binary(unsigned long val)
{
    if (val == 0) {
        printf("0b0\n");
        return;
    }
    char buf[66];
    int idx = 0;
    while (val > 0) {
        buf[idx++] = (val & 1) ? '1' : '0';
        val >>= 1;
    }
    printf("0b");
    for (int i = idx - 1; i >= 0; i--) putchar(buf[i]);
    putchar('\n');
}

static void print_result(double val)
{
    g_last = val;

    if (g_obase == 16) {
        printf("0x%lX\n", (unsigned long)val);
        return;
    } else if (g_obase == 8) {
        printf("0%lo\n", (unsigned long)val);
        return;
    } else if (g_obase == 2) {
        print_binary((unsigned long)val);
        return;
    }

    /* Decimal (obase=10) with scale */
    if (g_scale == 0 || floor(val) == val) {
        printf("%ld\n", (long)val);
    } else {
        switch (g_scale) {
        case 1: printf("%.1f\n", val); break;
        case 2: printf("%.2f\n", val); break;
        case 3: printf("%.3f\n", val); break;
        case 4: printf("%.4f\n", val); break;
        case 5: printf("%.5f\n", val); break;
        case 6: printf("%.6f\n", val); break;
        case 7: printf("%.7f\n", val); break;
        case 8: printf("%.8f\n", val); break;
        case 9: printf("%.9f\n", val); break;
        case 10: printf("%.10f\n", val); break;
        default: printf("%f\n", val); break;
        }
    }
}

static void evaluate_line(const char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#' || *line == '\n' || *line == '\r') return;

    if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
        exit(0);
    }

    parser_t p = { line, 0 };
    double res = parse_expr(&p);

    /* Check if it was an assignment like 'scale = 2' or 'a = 10' */
    const char *eq = strchr(line, '=');
    if (!eq) {
        print_result(res);
    }
}

/* ── REPL & CLI ───────────────────────────────────────────────────────────── */
static void run_repl(void)
{
    printf("AzamiOS bc v2.0 (Math & Radix Calculator)\n");
    printf("Type expressions (e.g. sqrt(144) + 2^4, 0x1A + 0b101, obase=16), or 'quit'.\n");

    char line[512];
    while (1) {
        printf("\033[38;2;203;166;247mbc>\033[0m ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';

        evaluate_line(line);
    }
}

int main(int argc, char **argv)
{
    set_var("pi", 3.141592653589793);
    set_var("e", 2.718281828459045);

    if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        evaluate_line(argv[2]);
        return 0;
    }

    if (isatty(STDIN_FILENO)) {
        run_repl();
    } else {
        char line[512];
        while (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            evaluate_line(line);
        }
    }

    return 0;
}
