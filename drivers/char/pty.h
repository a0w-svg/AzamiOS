/* ============================================================================
 * AzamiOS — UNIX98 Pseudo-Terminal (PTY) Subsystem Header
 * File: drivers/char/pty.h
 * ============================================================================ */
#pragma once

#include "../../fs/vfs.h"
#include "../../include/azami/types.h"

#define PTY_MAX_PAIRS 16
#define PTY_BUFFER_SIZE 4096

/* PTY ioctl commands */
#define TIOCGPTN    0x80045430  /* Get PTY number */
#define TIOCSPTLCK  0x40045431  /* Lock/unlock PTY */
#define TIOCGWINSZ  0x5413      /* Get window size */
#define TIOCSWINSZ  0x5414      /* Set window size */
#define TCGETS      0x5401      /* Get termios */
#define TCSETS      0x5402      /* Set termios */
#define TCSETSW     0x5403      /* Set termios (drain) */
#define TCSETSF     0x5404      /* Set termios (flush) */
#define TIOCSCTTY   0x540E      /* Set controlling tty */
#define TIOCGPGRP   0x540F      /* Get foreground process group */
#define TIOCSPGRP   0x5410      /* Set foreground process group */
#define FIONREAD    0x541B      /* Get bytes available */
#define TIOCGSID    0x5429      /* Get session ID */

#define TCSBRK      0x5409      /* Send break */
#define TCXONC      0x540A      /* Flow control */
#define TCFLSH      0x540B      /* Flush queue */
#define TIOCNOTTY   0x5422      /* Detach controlling tty */

#define TCIFLUSH    0
#define TCOFLUSH    1
#define TCIOFLUSH   2

struct pty_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

typedef struct {
    int id;
    bool allocated;
    bool locked;

    /* Master -> Slave ring buffer */
    u8 m2s_buf[PTY_BUFFER_SIZE];
    size_t m2s_head;
    size_t m2s_tail;
    size_t m2s_count;

    /* Slave -> Master ring buffer */
    u8 s2m_buf[PTY_BUFFER_SIZE];
    size_t s2m_head;
    size_t s2m_tail;
    size_t s2m_count;

    struct pty_winsize winsize;
    u8 termios[64];
    u32 pgrp;
} pty_pair_t;

void pty_init(void);
pty_pair_t *pty_get_pair(int id);
int pty_get_active_count(void);
file_operations_t *pty_get_slave_fops(void);
file_operations_t *pty_get_ptmx_fops(void);
