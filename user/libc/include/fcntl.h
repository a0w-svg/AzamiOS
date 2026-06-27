/**
 * fcntl.h  –  POSIX File Control Options
 */
#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0100
#define O_TRUNC  0x0200
#define O_APPEND 0x0400

int open(const char *path, int flags, ...);

#endif /* _FCNTL_H */
