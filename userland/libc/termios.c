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

void cfmakeraw(struct termios *termios_p)
{
    if (!termios_p) return;
    termios_p->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    termios_p->c_oflag &= ~OPOST;
    termios_p->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    termios_p->c_cflag &= ~(CSIZE | PARENB);
    termios_p->c_cflag |= CS8;
    termios_p->c_cc[VMIN] = 1;
    termios_p->c_cc[VTIME] = 0;
}

int tcsendbreak(int fd, int duration)
{
    return ioctl(fd, 0x5409 /* TCSBRK */, (unsigned long)duration);
}

int tcflush(int fd, int queue_selector)
{
    return ioctl(fd, 0x540B /* TCFLSH */, (unsigned long)queue_selector);
}

int tcflow(int fd, int action)
{
    return ioctl(fd, 0x540A /* TCXONC */, (unsigned long)action);
}

pid_t tcgetpgrp(int fd)
{
    pid_t pgrp = -1;
    if (ioctl(fd, 0x540F /* TIOCGPGRP */, (unsigned long)&pgrp) < 0) {
        return (pid_t)-1;
    }
    return pgrp;
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    return ioctl(fd, 0x5410 /* TIOCSPGRP */, (unsigned long)&pgrp);
}

pid_t tcgetsid(int fd)
{
    pid_t sid = -1;
    if (ioctl(fd, 0x5429 /* TIOCGSID */, (unsigned long)&sid) < 0) {
        return (pid_t)-1;
    }
    return sid;
}
