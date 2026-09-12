/* ============================================================================
 * AzamiOS Userspace — File Tree Walk (ftw.h)
 * File: userland/libc/include/ftw.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "sys/stat.h"

/* Type flags passed to the callback */
#define FTW_F    0  /* Regular file */
#define FTW_D    1  /* Directory */
#define FTW_DNR  2  /* Directory that cannot be read */
#define FTW_NS   3  /* Stat failed on object */
#define FTW_SL   4  /* Symbolic link */
#define FTW_DP   5  /* Directory whose subdirectories have been visited */
#define FTW_SLN  6  /* Symbolic link pointing to nonexistent file */

/* Flags controlling nftw() traversal */
#define FTW_PHYS   1  /* Physical walk: do not follow symbolic links */
#define FTW_MOUNT  2  /* Stay within the same mount point */
#define FTW_DEPTH  4  /* Post-order traversal: visit contents before directory */
#define FTW_CHDIR  8  /* Change current directory while traversing */

struct FTW {
    int base;   /* Offset of basename in path */
    int level;  /* Depth relative to walk root (0 for root) */
};

int ftw(const char *dirpath,
        int (*fn)(const char *fpath, const struct stat *sb, int typeflag),
        int nopenfd);

int nftw(const char *dirpath,
         int (*fn)(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf),
         int nopenfd, int flags);
