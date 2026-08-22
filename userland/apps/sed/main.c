/* ============================================================================
 * AzamiOS Userspace — POSIX Stream Editor (sed.elf)
 * File: userland/apps/sed/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/regex.h"

static int s_opt_n = 0;

static void process_line(char *line, const char *pattern, const char *repl, int global)
{
    regex_t reg;
    if (regcomp(&reg, pattern, REG_EXTENDED) != 0) {
        if (!s_opt_n) fputs(line, stdout);
        return;
    }

    char out[4096];
    char *out_p = out;
    char *src = line;
    regmatch_t match;
    int modified = 0;

    while (regexec(&reg, src, 1, &match, 0) == 0) {
        modified = 1;
        /* Copy up to start of match */
        size_t prefix_len = (size_t)match.rm_so;
        memcpy(out_p, src, prefix_len);
        out_p += prefix_len;

        /* Copy replacement */
        size_t rlen = strlen(repl);
        memcpy(out_p, repl, rlen);
        out_p += rlen;

        src += match.rm_eo;
        if (!global || match.rm_so == match.rm_eo) break;
    }

    /* Copy trailing text */
    strcpy(out_p, src);
    regfree(&reg);

    if (!s_opt_n || modified) {
        fputs(out, stdout);
    }
}

int main(int argc, char **argv)
{
    const char *script = NULL;
    int arg_idx = 1;

    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "-n") == 0) {
            s_opt_n = 1;
        } else if (strcmp(argv[arg_idx], "-e") == 0 && arg_idx + 1 < argc) {
            script = argv[++arg_idx];
        }
        arg_idx++;
    }

    if (!script && arg_idx < argc) {
        script = argv[arg_idx++];
    }

    if (!script) {
        fprintf(stderr, "usage: sed [-n] [-e script] 's/pattern/replacement/[g]' [file...]\n");
        return 1;
    }

    /* Parse substitution command s/pat/repl/[g] */
    char pat[512] = { 0 }, repl[512] = { 0 };
    int global = 0;

    if (script[0] == 's' && script[1] != '\0') {
        char delim = script[1];
        const char *p1 = script + 2;
        const char *p2 = strchr(p1, delim);
        if (p2) {
            size_t plen = (size_t)(p2 - p1);
            strncpy(pat, p1, plen);
            pat[plen] = '\0';

            const char *p3 = p2 + 1;
            const char *p4 = strchr(p3, delim);
            if (p4) {
                size_t rlen = (size_t)(p4 - p3);
                strncpy(repl, p3, rlen);
                repl[rlen] = '\0';
                if (strchr(p4 + 1, 'g')) global = 1;
            } else {
                strcpy(repl, p3);
            }
        }
    } else {
        /* Default literal search */
        strcpy(pat, script);
    }

    FILE *in = stdin;
    if (arg_idx < argc) {
        in = fopen(argv[arg_idx], "r");
        if (!in) {
            fprintf(stderr, "sed: %s: No such file\n", argv[arg_idx]);
            return 1;
        }
    }

    char line_buf[2048];
    while (fgets(line_buf, sizeof(line_buf), in)) {
        process_line(line_buf, pat, repl, global);
    }

    if (in != stdin) fclose(in);
    return 0;
}
