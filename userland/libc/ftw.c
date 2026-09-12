/* ============================================================================
 * AzamiOS Userspace — File Tree Walk Implementation (ftw.c)
 * File: userland/libc/ftw.c
 * ============================================================================ */

#include "include/ftw.h"
#include "include/dirent.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/unistd.h"
#include "include/errno.h"
#include "include/stdio.h"

#define FTW_PATH_MAX 4096

static int do_nftw(char *path,
                   int (*fn)(const char *, const struct stat *, int, struct FTW *),
                   int nopenfd,
                   int flags,
                   int level,
                   dev_t root_dev)
{
    struct stat st;
    int typeflag;
    int ret;

    int stat_res;
    if (flags & FTW_PHYS) {
        stat_res = lstat(path, &st);
    } else {
        stat_res = stat(path, &st);
    }

    /* Find base offset */
    char *slash = strrchr(path, '/');
    int base = slash ? (int)(slash - path + 1) : 0;

    struct FTW ftwbuf = {
        .base  = base,
        .level = level,
    };

    if (stat_res != 0) {
        return fn(path, &st, FTW_NS, &ftwbuf);
    }

    if ((flags & FTW_MOUNT) && level > 0 && st.st_dev != root_dev) {
        return 0; /* Skip foreign mount points */
    }

    if (S_ISLNK(st.st_mode)) {
        typeflag = FTW_SL;
        return fn(path, &st, typeflag, &ftwbuf);
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dirp = opendir(path);
        if (!dirp) {
            return fn(path, &st, FTW_DNR, &ftwbuf);
        }

        if (!(flags & FTW_DEPTH)) {
            ret = fn(path, &st, FTW_D, &ftwbuf);
            if (ret != 0) {
                closedir(dirp);
                return ret;
            }
        }

        size_t path_len = strlen(path);
        struct dirent *de;
        while ((de = readdir(dirp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }

            size_t name_len = strlen(de->d_name);
            if (path_len + name_len + 2 >= FTW_PATH_MAX) {
                continue;
            }

            /* Append entry */
            if (path[path_len - 1] == '/') {
                snprintf(path + path_len, FTW_PATH_MAX - path_len, "%s", de->d_name);
            } else {
                snprintf(path + path_len, FTW_PATH_MAX - path_len, "/%s", de->d_name);
            }

            ret = do_nftw(path, fn, nopenfd, flags, level + 1, root_dev);
            path[path_len] = '\0'; /* Restore path */

            if (ret != 0) {
                closedir(dirp);
                return ret;
            }
        }

        closedir(dirp);

        if (flags & FTW_DEPTH) {
            ret = fn(path, &st, FTW_DP, &ftwbuf);
            if (ret != 0) return ret;
        }

        return 0;
    }

    return fn(path, &st, FTW_F, &ftwbuf);
}

int nftw(const char *dirpath,
         int (*fn)(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf),
         int nopenfd, int flags)
{
    if (!dirpath || !fn) {
        errno = EINVAL;
        return -1;
    }

    char path[FTW_PATH_MAX];
    strncpy(path, dirpath, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    struct stat st;
    if (stat(dirpath, &st) != 0) {
        return -1;
    }

    return do_nftw(path, fn, nopenfd, flags, 0, st.st_dev);
}

/* ── Legacy ftw() Wrapper ─────────────────────────────────────────────────── */

static int (*g_ftw_fn)(const char *, const struct stat *, int) = NULL;

static int ftw_callback_adapter(const char *fpath, const struct stat *sb,
                                int typeflag, struct FTW *ftwbuf)
{
    (void)ftwbuf;
    if (g_ftw_fn) {
        return g_ftw_fn(fpath, sb, typeflag);
    }
    return 0;
}

int ftw(const char *dirpath,
        int (*fn)(const char *fpath, const struct stat *sb, int typeflag),
        int nopenfd)
{
    g_ftw_fn = fn;
    return nftw(dirpath, ftw_callback_adapter, nopenfd, 0);
}
