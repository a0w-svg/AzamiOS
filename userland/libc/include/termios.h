/* ============================================================================
 * AzamiOS Userspace — Terminal I/O Header (termios.h)
 * File: userland/libc/include/termios.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

#define NCCS 32

struct termios {
    tcflag_t c_iflag;      /* input mode flags */
    tcflag_t c_oflag;      /* output mode flags */
    tcflag_t c_cflag;      /* control mode flags */
    tcflag_t c_lflag;      /* local mode flags */
    cc_t     c_line;       /* line discipline */
    cc_t     c_cc[NCCS];   /* control characters */
    speed_t  c_ispeed;     /* input speed */
    speed_t  c_ospeed;     /* output speed */
};

/* c_iflag */
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

/* c_oflag */
#define OPOST   0000001
#define ONLCR   0000004

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0010000

/* Actions for tcsetattr */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

/* Baud rates */
#define B0      0000000
#define B50     0000001
#define B75     0000002
#define B110    0000003
#define B134    0000004
#define B150    0000005
#define B200    0000006
#define B300    0000007
#define B600    0000010
#define B1200   0000011
#define B1800   0000012
#define B2400   0000013
#define B4800   0000014
#define B9600   0000015
#define B19200  0000016
#define B38400  0000017
#define B57600  0010001
#define B115200 0010002

/* Control characters */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11

/* c_cflag */
#define CSIZE    0000060
#define CS5      0000000
#define CS6      0000020
#define CS7      0000040
#define CS8      0000060
#define CSTOPB   0000100
#define CREAD    0000200
#define PARENB   0000400
#define PARODD   0001000
#define HUPCL    0002000
#define CLOCAL   0004000

/* Flush / Flow */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
speed_t cfgetispeed(const struct termios *termios_p);
speed_t cfgetospeed(const struct termios *termios_p);
int cfsetispeed(struct termios *termios_p, speed_t speed);
int cfsetospeed(struct termios *termios_p, speed_t speed);
void cfmakeraw(struct termios *termios_p);
int tcsendbreak(int fd, int duration);
int tcflush(int fd, int queue_selector);
int tcflow(int fd, int action);

#ifndef _STRUCT_WINSIZE_DEFINED
#define _STRUCT_WINSIZE_DEFINED
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
#endif

pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
pid_t tcgetsid(int fd);
