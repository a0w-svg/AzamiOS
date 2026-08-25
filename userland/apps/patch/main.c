/* ============================================================================
 * AzamiOS — patch (Apply a unified diff patch)
 * File: userland/apps/patch/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 16384
#define MAX_LINE_LEN 2048

static char *lines[MAX_LINES];
static int line_count = 0;

int main(int argc, char **argv)
{
    const char *target_path = NULL;
    const char *patch_path = NULL;
    int strip_p = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-p", 2) == 0) {
            strip_p = atoi(argv[i] + 2);
        } else if (!target_path) {
            target_path = argv[i];
        } else if (!patch_path) {
            patch_path = argv[i];
        }
    }

    FILE *patch_fp = (patch_path && strcmp(patch_path, "-") != 0) ? fopen(patch_path, "r") : stdin;
    if (!patch_fp) {
        perror(patch_path ? patch_path : "patch");
        return 1;
    }

    char buf[MAX_LINE_LEN];
    char detected_target[256] = {0};

    /* Scan header if target not specified */
    if (!target_path) {
        while (fgets(buf, sizeof(buf), patch_fp)) {
            if (strncmp(buf, "+++ ", 4) == 0) {
                char *fn = buf + 4;
                while (*fn == ' ' || *fn == '\t') fn++;
                char *tab = strchr(fn, '\t');
                if (tab) *tab = '\0';
                char *nl = strchr(fn, '\n');
                if (nl) *nl = '\0';
                char *cr = strchr(fn, '\r');
                if (cr) *cr = '\0';

                const char *p = fn;
                for (int s = 0; s < strip_p && p; s++) {
                    char *slash = strchr(p, '/');
                    if (slash) p = slash + 1;
                }
                strncpy(detected_target, p, sizeof(detected_target) - 1);
                target_path = detected_target;
                break;
            }
        }
        rewind(patch_fp);
    }

    if (!target_path || !target_path[0]) {
        fprintf(stderr, "patch: cannot determine target file\n");
        if (patch_fp != stdin) fclose(patch_fp);
        return 1;
    }

    /* Read original target file */
    FILE *target_fp = fopen(target_path, "r");
    if (target_fp) {
        while (fgets(buf, sizeof(buf), target_fp) && line_count < MAX_LINES) {
            size_t l = strlen(buf);
            if (l > 0 && buf[l - 1] == '\n') buf[--l] = '\0';
            if (l > 0 && buf[l - 1] == '\r') buf[--l] = '\0';
            lines[line_count++] = strdup(buf);
        }
        fclose(target_fp);
    }

    /* Read patch and apply */
    char *out_lines[MAX_LINES];
    int out_count = 0;
    int src_idx = 0;

    while (fgets(buf, sizeof(buf), patch_fp) && out_count < MAX_LINES) {
        if (strncmp(buf, "--- ", 4) == 0 || strncmp(buf, "+++ ", 4) == 0) continue;
        if (strncmp(buf, "@@", 2) == 0) continue;

        size_t l = strlen(buf);
        if (l > 0 && buf[l - 1] == '\n') buf[--l] = '\0';
        if (l > 0 && buf[l - 1] == '\r') buf[--l] = '\0';

        char prefix = buf[0];
        const char *content = buf + 1;

        if (prefix == ' ') {
            out_lines[out_count++] = strdup(content);
            src_idx++;
        } else if (prefix == '+') {
            out_lines[out_count++] = strdup(content);
        } else if (prefix == '-') {
            src_idx++;
        }
    }

    if (patch_fp != stdin) fclose(patch_fp);

    /* Write modified lines back to target file */
    target_fp = fopen(target_path, "w");
    if (!target_fp) {
        perror(target_path);
        return 1;
    }

    for (int i = 0; i < out_count; i++) {
        fprintf(target_fp, "%s\n", out_lines[i]);
        free(out_lines[i]);
    }
    fclose(target_fp);

    for (int i = 0; i < line_count; i++) free(lines[i]);

    printf("patching file %s\n", target_path);
    return 0;
}
