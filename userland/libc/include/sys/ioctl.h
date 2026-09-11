/* ============================================================================
 * AzamiOS Userspace — Device Control Header (sys/ioctl.h)
 * File: userland/libc/include/sys/ioctl.h
 * ============================================================================ */
#pragma once

#include <stddef.h>

#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS   14
#define _IOC_DIRBITS    2

#define _IOC_NRMASK     ((1U << _IOC_NRBITS)-1)
#define _IOC_TYPEMASK   ((1U << _IOC_TYPEBITS)-1)
#define _IOC_SIZEMASK   ((1U << _IOC_SIZEBITS)-1)
#define _IOC_DIRMASK    ((1U << _IOC_DIRBITS)-1)

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT+_IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT+_IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT+_IOC_SIZEBITS)

#define _IOC_NONE       0U
#define _IOC_WRITE      1U
#define _IOC_READ       2U

#define _IOC(dir,type,nr,size) \
        (((unsigned long)(dir)  << _IOC_DIRSHIFT) | \
         ((unsigned long)(type) << _IOC_TYPESHIFT) | \
         ((unsigned long)(nr)   << _IOC_NRSHIFT) | \
         ((unsigned long)(size) << _IOC_SIZESHIFT))

#define _IO(type,nr)            _IOC(_IOC_NONE,(type),(nr),0)
#define _IOR(type,nr,size)      _IOC(_IOC_READ,(type),(nr),sizeof(size))
#define _IOW(type,nr,size)      _IOC(_IOC_WRITE,(type),(nr),sizeof(size))
#ifndef _STRUCT_WINSIZE_DEFINED
#define _STRUCT_WINSIZE_DEFINED
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#endif

/* Terminal & TTY ioctls */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD    0x541B
#define TIOCNOTTY   0x5422
#define TIOCGSID    0x5429

/* UNIX98 PTY ioctls */
#define TIOCGPTN    0x80045430  /* Get PTY number */
#define TIOCSPTLCK  0x40045431  /* Lock/unlock PTY */

/* Socket / File ioctls */
#define FIONBIO     0x5421
#define FIONCLEX    0x5450
#define FIOCLEX     0x5451
#define FIOASYNC    0x5452

int ioctl(int fd, unsigned long request, ...);
