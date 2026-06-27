/**
 * dirent.c — AzamiOS libc: directory iteration
 */
#include "include/dirent.h"
#include "include/stdlib.h"
#include "include/string.h"
#include <unistd.h>
#include <fcntl.h>

DIR *opendir(const char *path) {
    if (!path) return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    
    DIR *dirp = (DIR *)malloc(sizeof(DIR));
    if (!dirp) { close(fd); return 0; }
    dirp->fd = fd;
    dirp->index = 0;
    return dirp;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp || dirp->fd < 0) return 0;
    
    /* Read 128-byte raw filename entry from VFS */
    char name_buf[128];
    ssize_t n = read(dirp->fd, name_buf, 128);
    if (n <= 0 || name_buf[0] == '\0') return 0;
    
    name_buf[127] = '\0';
    dirp->current.d_ino = ++dirp->index;
    strncpy(dirp->current.d_name, name_buf, NAME_MAX);
    dirp->current.d_name[NAME_MAX] = '\0';
    
    return &dirp->current;
}

int closedir(DIR *dirp) {
    if (!dirp) return -1;
    int res = close(dirp->fd);
    free(dirp);
    return res;
}

void rewinddir(DIR *dirp) {
    if (!dirp) return;
    dirp->index = 0;
    /* Close and reopen or seek to 0 if supported */
}
