/* ============================================================================
 * AzamiOS Userspace — POSIX Terminal Control Implementation (termios.c)
 * File: userland/libc/termios.c
 * ============================================================================ */

#include "include/termios.h"
#include "include/sys/syscall.h"
#include "include/sys/ioctl.h"

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404

int tcgetattr(int fd, struct termios *termios_p)
{
    return ioctl(fd, TCGETS, (unsigned long)termios_p);
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p)
{
    unsigned long cmd = TCSETS;
    if (optional_actions == TCSADRAIN) cmd = TCSETSW;
    else if (optional_actions == TCSAFLUSH) cmd = TCSETSF;

    return ioctl(fd, cmd, (unsigned long)termios_p);
}

speed_t cfgetispeed(const struct termios *termios_p)
{
    return termios_p ? termios_p->c_ispeed : 0;
}

speed_t cfgetospeed(const struct termios *termios_p)
{
    return termios_p ? termios_p->c_ospeed : 0;
}

int cfsetispeed(struct termios *termios_p, speed_t speed)
{
    if (!termios_p) return -1;
    termios_p->c_ispeed = speed;
    return 0;
}

int cfsetospeed(struct termios *termios_p, speed_t speed)
{
    if (!termios_p) return -1;
    termios_p->c_ospeed = speed;
    return 0;
}
