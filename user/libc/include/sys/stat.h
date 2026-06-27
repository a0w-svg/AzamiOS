/**
 * sys/stat.h  –  POSIX File Status Header for AzamiOS Newlib
 */
#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

/* File mode bits */
#define S_IFMT   00170000
#define S_IFIFO  00010000
#define S_IFCHR  00020000
#define S_IFDIR  00040000
#define S_IFBLK  00060000
#define S_IFREG  00100000

#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

struct stat {
    dev_t   st_dev;
    ino_t   st_ino;
    mode_t  st_mode;
    nlink_t st_nlink;
    uid_t   st_uid;
    gid_t   st_gid;
    dev_t   st_rdev;
    off_t   st_size;
};

int stat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);

#endif /* _SYS_STAT_H */
