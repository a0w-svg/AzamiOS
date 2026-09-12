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

int readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result)
{
    if (!dirp || !entry || !result) return 22; /* EINVAL */
    struct dirent *d = readdir(dirp);
    if (!d) {
        *result = NULL;
        return 0;
    }
    memcpy(entry, d, sizeof(struct dirent));
    *result = entry;
    return 0;
}

int alphasort(const struct dirent **a, const struct dirent **b)
{
    if (!a || !*a || !b || !*b) return 0;
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int _version_cmp(const char *s1, const char *s2)
{
    while (*s1 && *s2) {
        if (*s1 >= '0' && *s1 <= '9' && *s2 >= '0' && *s2 <= '9') {
            unsigned long n1 = strtoul(s1, (char **)&s1, 10);
            unsigned long n2 = strtoul(s2, (char **)&s2, 10);
            if (n1 != n2) return (n1 < n2) ? -1 : 1;
        } else {
            if (*s1 != *s2) return (*(unsigned char *)s1 - *(unsigned char *)s2);
            s1++;
            s2++;
        }
    }
    return (*(unsigned char *)s1 - *(unsigned char *)s2);
}

int versionsort(const struct dirent **a, const struct dirent **b)
{
    if (!a || !*a || !b || !*b) return 0;
    return _version_cmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **))
{
    if (!dirp || !namelist) return -1;
    DIR *d = opendir(dirp);
    if (!d) return -1;

    size_t count = 0;
    size_t cap = 16;
    struct dirent **list = (struct dirent **)malloc(cap * sizeof(struct dirent *));
    if (!list) {
        closedir(d);
        return -1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (filter && !filter(de)) continue;

        struct dirent *copy = (struct dirent *)malloc(sizeof(struct dirent));
        if (!copy) {
            for (size_t i = 0; i < count; i++) free(list[i]);
            free(list);
            closedir(d);
            return -1;
        }
        memcpy(copy, de, sizeof(struct dirent));

        if (count >= cap) {
            cap *= 2;
            struct dirent **nl = (struct dirent **)realloc(list, cap * sizeof(struct dirent *));
            if (!nl) {
                free(copy);
                for (size_t i = 0; i < count; i++) free(list[i]);
                free(list);
                closedir(d);
                return -1;
            }
            list = nl;
        }
        list[count++] = copy;
    }
    closedir(d);

    if (compar && count > 1) {
        qsort(list, count, sizeof(struct dirent *),
              (int (*)(const void *, const void *))compar);
    }

    *namelist = list;
    return (int)count;
}
