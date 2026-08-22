/* ============================================================================
 * AzamiOS Userspace — Pathname Expansion Engine (glob.c)
 * File: userland/libc/glob.c
 * ============================================================================ */

#include "include/glob.h"
#include "include/fnmatch.h"
#include "include/dirent.h"
#include "include/sys/stat.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdio.h"
#include "include/unistd.h"

static void add_path(glob_t *pglob, const char *path)
{
    size_t new_count = pglob->gl_pathc + 1;
    char **new_v = (char **)realloc(pglob->gl_pathv, (new_count + 1) * sizeof(char *));
    if (!new_v) return;

    pglob->gl_pathv = new_v;
    pglob->gl_pathv[pglob->gl_pathc] = strdup(path);
    pglob->gl_pathc = new_count;
    pglob->gl_pathv[pglob->gl_pathc] = NULL;
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *epath, int eerrno), glob_t *pglob)
{
    (void)errfunc;
    if (!pattern || !pglob) return GLOB_ABORTED;

    if (!(flags & GLOB_APPEND)) {
        pglob->gl_pathc = 0;
        pglob->gl_pathv = NULL;
        pglob->gl_offs  = 0;
    }

    /* Extract directory part and filename pattern */
    char dir_path[512];
    const char *pat_base = pattern;
    const char *last_slash = strrchr(pattern, '/');

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - pattern);
        if (dlen == 0) {
            strcpy(dir_path, "/");
        } else {
            strncpy(dir_path, pattern, dlen);
            dir_path[dlen] = '\0';
        }
        pat_base = last_slash + 1;
    } else {
        strcpy(dir_path, ".");
    }

    DIR *d = opendir(dir_path);
    if (!d) {
        if (flags & GLOB_NOCHECK) {
            add_path(pglob, pattern);
            return 0;
        }
        return GLOB_NOMATCH;
    }

    struct dirent *de;
    size_t initial_count = pglob->gl_pathc;
    int fnm_flags = FNM_PATHNAME;
    if (flags & GLOB_PERIOD) fnm_flags |= FNM_PERIOD;
    if (flags & GLOB_NOESCAPE) fnm_flags |= FNM_NOESCAPE;

    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            if (strcmp(pat_base, ".") != 0 && strcmp(pat_base, "..") != 0) continue;
        }

        if (fnmatch(pat_base, de->d_name, fnm_flags) == 0) {
            char full[1024];
            if (last_slash) {
                if (strcmp(dir_path, "/") == 0) {
                    snprintf(full, sizeof(full), "/%s", de->d_name);
                } else {
                    snprintf(full, sizeof(full), "%s/%s", dir_path, de->d_name);
                }
            } else {
                snprintf(full, sizeof(full), "%s", de->d_name);
            }

            if (flags & GLOB_MARK) {
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                    strcat(full, "/");
                }
            }

            add_path(pglob, full);
        }
    }
    closedir(d);

    if (pglob->gl_pathc == initial_count) {
        if (flags & GLOB_NOCHECK) {
            add_path(pglob, pattern);
            return 0;
        }
        return GLOB_NOMATCH;
    }

    return 0;
}

void globfree(glob_t *pglob)
{
    if (!pglob || !pglob->gl_pathv) return;
    for (size_t i = 0; i < pglob->gl_pathc; i++) {
        if (pglob->gl_pathv[i]) free(pglob->gl_pathv[i]);
    }
    free(pglob->gl_pathv);
    pglob->gl_pathv = NULL;
    pglob->gl_pathc = 0;
}
