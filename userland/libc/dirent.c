/* ============================================================================
 * AzamiOS Userspace — Directory Entry Handling (dirent.c)
 * File: userland/libc/dirent.c
 * ============================================================================ */

#include "include/dirent.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/fcntl.h"
#include "include/unistd.h"

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

extern int sys_getdents64(int fd, void *dirp, size_t count);

DIR *opendir(const char *name)
{
    if (!name) return NULL;
    int fd = open(name, O_RDONLY, 0);
    if (fd < 0) return NULL;

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (!dir) {
        close(fd);
        return NULL;
    }
    dir->fd = fd;
    dir->buf_pos = 0;
    dir->buf_len = 0;
    return dir;
}

static struct dirent g_static_dirent;

struct dirent *readdir(DIR *dirp)
{
    if (!dirp || dirp->fd < 0) return NULL;

    if (dirp->buf_pos >= dirp->buf_len) {
        int n = sys_getdents64(dirp->fd, dirp->buf, sizeof(dirp->buf));
        if (n <= 0) return NULL;
        dirp->buf_len = n;
        dirp->buf_pos = 0;
    }

    struct linux_dirent64 *d = (struct linux_dirent64 *)(dirp->buf + dirp->buf_pos);
    if (d->d_reclen == 0) return NULL;

    g_static_dirent.d_ino = d->d_ino;
    g_static_dirent.d_off = d->d_off;
    g_static_dirent.d_reclen = d->d_reclen;
    g_static_dirent.d_type = d->d_type;
    strncpy(g_static_dirent.d_name, d->d_name, sizeof(g_static_dirent.d_name) - 1);
    g_static_dirent.d_name[sizeof(g_static_dirent.d_name) - 1] = '\0';

    dirp->buf_pos += d->d_reclen;
    return &g_static_dirent;
}

int closedir(DIR *dirp)
{
    if (!dirp) return -1;
    int r = close(dirp->fd);
    free(dirp);
    return r;
}

void rewinddir(DIR *dirp)
{
    if (dirp) {
        lseek(dirp->fd, 0, SEEK_SET);
        dirp->buf_pos = 0;
        dirp->buf_len = 0;
    }
}

long telldir(DIR *dirp)
{
    if (!dirp) return -1;
    return (long)lseek(dirp->fd, 0, SEEK_CUR);
}

void seekdir(DIR *dirp, long loc)
{
    if (dirp && loc >= 0) {
        lseek(dirp->fd, (ssize_t)loc, SEEK_SET);
        dirp->buf_pos = 0;
        dirp->buf_len = 0;
    }
}
