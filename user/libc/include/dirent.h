/**
 * dirent.h — AzamiOS libc: POSIX directory iteration
 *
 * Directory iteration is backed by the kernel VFS read syscall.
 * A directory "file" yields a stream of nul-terminated filenames,
 * one per 128-byte record (matching fs_node_t.name).
 */
#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

#define NAME_MAX 127

struct dirent {
    ino_t  d_ino;           /* inode number   */
    char   d_name[NAME_MAX + 1]; /* file name  */
};

typedef struct {
    int    fd;              /* underlying file descriptor      */
    int    index;           /* current entry index             */
    struct dirent current;  /* last entry filled by readdir()  */
} DIR;

DIR           *opendir (const char *path);
struct dirent *readdir (DIR *dirp);
int            closedir(DIR *dirp);
void           rewinddir(DIR *dirp);

#endif /* _DIRENT_H */
