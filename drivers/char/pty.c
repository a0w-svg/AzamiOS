/* ============================================================================
 * AzamiOS — UNIX98 Pseudo-Terminal (PTY) Subsystem Implementation
 * File: drivers/char/pty.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "pty.h"
#include "../../fs/vfs.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/sched/sched.h"
#include "../../include/azami/defs.h"

static spinlock_t g_pty_lock = SPINLOCK_INIT;
static pty_pair_t g_pty_pairs[PTY_MAX_PAIRS];

static void pty_init_termios(pty_pair_t *pty)
{
    memset(pty->termios, 0, sizeof(pty->termios));
    *(u32 *)&pty->termios[0]  = 0x0100; /* ICRNL */
    *(u32 *)&pty->termios[4]  = 0x0005; /* OPOST | ONLCR */
    *(u32 *)&pty->termios[8]  = 0x00BF; /* CS8 | CREAD | B38400 */
    *(u32 *)&pty->termios[12] = 0x0A3B; /* ISIG | ICANON | ECHO | ECHOE | ECHOK */
    pty->termios[16] = 0;               /* c_line */
    pty->termios[17 + 0] = 0x03;        /* VINTR = ^C */
    pty->termios[17 + 1] = 0x1C;        /* VQUIT = ^\ */
    pty->termios[17 + 2] = 0x7F;        /* VERASE = DEL/backspace */
    pty->termios[17 + 3] = 0x15;        /* VKILL = ^U */
    pty->termios[17 + 4] = 0x04;        /* VEOF = ^D */
    pty->termios[17 + 5] = 0;           /* VTIME */
    pty->termios[17 + 6] = 1;           /* VMIN */
    pty->termios[17 + 7] = 0;           /* VSWTC */
    pty->termios[17 + 8] = 0x11;        /* VSTART = ^Q */
    pty->termios[17 + 9] = 0x13;        /* VSTOP = ^S */
    pty->termios[17 + 10] = 0x1A;       /* VSUSP = ^Z */
    pty->termios[17 + 11] = 0;          /* VEOL */
    pty->pgrp = 0;
}

pty_pair_t *pty_get_pair(int id)
{
    if (id < 0 || id >= PTY_MAX_PAIRS) return NULL;
    if (!g_pty_pairs[id].allocated) return NULL;
    return &g_pty_pairs[id];
}

int pty_get_active_count(void)
{
    int cnt = 0;
    spinlock_lock(&g_pty_lock);
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (g_pty_pairs[i].allocated) cnt++;
    }
    spinlock_unlock(&g_pty_lock);
    return cnt;
}

/* ── Master operations ───────────────────────────────────────────────────── */

static s64 ptm_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    spinlock_lock(&g_pty_lock);
    if (pty->s2m_count == 0) {
        spinlock_unlock(&g_pty_lock);
        return (filp->f_flags & O_NONBLOCK) ? -(s64)EAGAIN : 0;
    }

    size_t copied = 0;
    u8 *out = (u8 *)buf;
    while (copied < len && pty->s2m_count > 0) {
        out[copied++] = pty->s2m_buf[pty->s2m_tail];
        pty->s2m_tail = (pty->s2m_tail + 1) % PTY_BUFFER_SIZE;
        pty->s2m_count--;
    }
    spinlock_unlock(&g_pty_lock);

    return (s64)copied;
}

static s64 ptm_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    spinlock_lock(&g_pty_lock);
    size_t written = 0;
    const u8 *in = (const u8 *)buf;
    while (written < len && pty->m2s_count < PTY_BUFFER_SIZE) {
        pty->m2s_buf[pty->m2s_head] = in[written++];
        pty->m2s_head = (pty->m2s_head + 1) % PTY_BUFFER_SIZE;
        pty->m2s_count++;
    }
    spinlock_unlock(&g_pty_lock);

    return (s64)written;
}

static s64 ptm_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    if (!filp || !filp->private_data) return -(s64)EINVAL;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    switch (cmd) {
    case TIOCGPTN: {
        int id = pty->id;
        if (copy_to_user((void *)(uintptr_t)arg, &id, sizeof(int)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TIOCSPTLCK: {
        int lock = 0;
        if (copy_from_user(&lock, (const void *)(uintptr_t)arg, sizeof(int)) != 0)
            return -(s64)EFAULT;
        pty->locked = (lock != 0);
        return 0;
    }
    case TIOCGWINSZ: {
        if (copy_to_user((void *)(uintptr_t)arg, &pty->winsize, sizeof(struct pty_winsize)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TIOCSWINSZ: {
        if (copy_from_user(&pty->winsize, (const void *)(uintptr_t)arg, sizeof(struct pty_winsize)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case FIONREAD: {
        int count = (int)pty->s2m_count;
        if (copy_to_user((void *)(uintptr_t)arg, &count, sizeof(int)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TCGETS: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        if (copy_to_user((void *)(uintptr_t)arg, pty->termios, 60) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        if (copy_from_user(pty->termios, (const void *)(uintptr_t)arg, 60) != 0)
            return -(s64)EFAULT;
        if (cmd == TCSETSF) {
            spinlock_lock(&g_pty_lock);
            pty->s2m_head = pty->s2m_tail = pty->s2m_count = 0;
            spinlock_unlock(&g_pty_lock);
        }
        return 0;
    }
    case TCFLSH: {
        spinlock_lock(&g_pty_lock);
        if (arg == TCIFLUSH || arg == TCIOFLUSH) {
            pty->s2m_head = pty->s2m_tail = pty->s2m_count = 0;
        }
        if (arg == TCOFLUSH || arg == TCIOFLUSH) {
            pty->m2s_head = pty->m2s_tail = pty->m2s_count = 0;
        }
        spinlock_unlock(&g_pty_lock);
        return 0;
    }
    case TIOCSPGRP: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        int pgrp = 0;
        if (copy_from_user(&pgrp, (const void *)(uintptr_t)arg, sizeof(int)) != 0)
            return -(s64)EFAULT;
        pty->pgrp = (u32)pgrp;
        return 0;
    }
    case TIOCGPGRP: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        process_t *proc = sched_current_process();
        int pgid = pty->pgrp ? (int)pty->pgrp : (proc ? (int)proc->pgid : 1);
        if (copy_to_user((void *)(uintptr_t)arg, &pgid, sizeof(int)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TIOCGSID: {
        process_t *proc = sched_current_process();
        int sid = proc ? (int)proc->pid : 1;
        if (copy_to_user((void *)(uintptr_t)arg, &sid, sizeof(int)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    default:
        return -(s64)ENOTTY;
    }
}

static s64 ptm_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (!filp || !filp->private_data) return 0;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    spinlock_lock(&g_pty_lock);
    pty->allocated = false;
    pty->locked = true;
    pty->m2s_head = pty->m2s_tail = pty->m2s_count = 0;
    pty->s2m_head = pty->s2m_tail = pty->s2m_count = 0;
    spinlock_unlock(&g_pty_lock);

    return 0;
}

static file_operations_t g_ptm_fops = {
    .read = ptm_read,
    .write = ptm_write,
    .ioctl = ptm_ioctl,
    .release = ptm_release,
};

static s64 ptmx_open(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (!filp) return -(s64)EINVAL;

    spinlock_lock(&g_pty_lock);
    int free_idx = -1;
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (!g_pty_pairs[i].allocated) {
            free_idx = i;
            break;
        }
    }

    if (free_idx < 0) {
        spinlock_unlock(&g_pty_lock);
        return -(s64)ENOSPC;
    }

    pty_pair_t *pty = &g_pty_pairs[free_idx];
    pty->id = free_idx;
    pty->allocated = true;
    pty->locked = true; /* Locked by default until TIOCSPTLCK 0 */
    pty->m2s_head = pty->m2s_tail = pty->m2s_count = 0;
    pty->s2m_head = pty->s2m_tail = pty->s2m_count = 0;
    pty->winsize.ws_row = 25;
    pty->winsize.ws_col = 80;
    pty->winsize.ws_xpixel = 640;
    pty->winsize.ws_ypixel = 400;
    pty_init_termios(pty);

    filp->private_data = pty;
    filp->f_op = &g_ptm_fops;
    spinlock_unlock(&g_pty_lock);

    return 0;
}

static file_operations_t g_ptmx_fops = {
    .open = ptmx_open,
};

static s64 pts_open(inode_t *inode, file_t *filp)
{
    if (!filp) return -(s64)EINVAL;
    if (!inode || !inode->i_private) return -(s64)ENXIO;
    pty_pair_t *pty = (pty_pair_t *)inode->i_private;
    if (pty->locked) return -(s64)EIO;
    filp->private_data = pty;
    return 0;
}

static s64 pts_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp) {
        filp->private_data = NULL;
    }
    return 0;
}

static s64 pts_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    spinlock_lock(&g_pty_lock);
    if (pty->m2s_count == 0) {
        spinlock_unlock(&g_pty_lock);
        return (filp->f_flags & O_NONBLOCK) ? -(s64)EAGAIN : 0;
    }

    size_t copied = 0;
    u8 *out = (u8 *)buf;
    while (copied < len && pty->m2s_count > 0) {
        out[copied++] = pty->m2s_buf[pty->m2s_tail];
        pty->m2s_tail = (pty->m2s_tail + 1) % PTY_BUFFER_SIZE;
        pty->m2s_count--;
    }
    spinlock_unlock(&g_pty_lock);

    return (s64)copied;
}

static s64 pts_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    spinlock_lock(&g_pty_lock);
    size_t written = 0;
    const u8 *in = (const u8 *)buf;
    while (written < len && pty->s2m_count < PTY_BUFFER_SIZE) {
        pty->s2m_buf[pty->s2m_head] = in[written++];
        pty->s2m_head = (pty->s2m_head + 1) % PTY_BUFFER_SIZE;
        pty->s2m_count++;
    }
    spinlock_unlock(&g_pty_lock);

    return (s64)written;
}

static s64 pts_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    if (!filp || !filp->private_data) return -(s64)EINVAL;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    switch (cmd) {
    case TIOCGWINSZ: {
        if (copy_to_user((void *)(uintptr_t)arg, &pty->winsize, sizeof(struct pty_winsize)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TIOCSWINSZ: {
        if (copy_from_user(&pty->winsize, (const void *)(uintptr_t)arg, sizeof(struct pty_winsize)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case FIONREAD: {
        int count = (int)pty->m2s_count;
        if (copy_to_user((void *)(uintptr_t)arg, &count, sizeof(int)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TCGETS: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        if (copy_to_user((void *)(uintptr_t)arg, pty->termios, 60) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        if (copy_from_user(pty->termios, (const void *)(uintptr_t)arg, 60) != 0)
            return -(s64)EFAULT;
        if (cmd == TCSETSF) {
            spinlock_lock(&g_pty_lock);
            pty->s2m_head = pty->s2m_tail = pty->s2m_count = 0;
            spinlock_unlock(&g_pty_lock);
        }
        return 0;
    }
    case TCFLSH: {
        spinlock_lock(&g_pty_lock);
        if (arg == TCIFLUSH || arg == TCIOFLUSH) {
            pty->m2s_head = pty->m2s_tail = pty->m2s_count = 0;
        }
        if (arg == TCOFLUSH || arg == TCIOFLUSH) {
            pty->s2m_head = pty->s2m_tail = pty->s2m_count = 0;
        }
        spinlock_unlock(&g_pty_lock);
        return 0;
    }
    case TCSBRK:
    case TCXONC:
        return 0;
    case TIOCSCTTY: {
        process_t *proc = sched_current_process();
        if (proc) {
            proc->sid = proc->pid;
        }
        return 0;
    }
    case TIOCNOTTY:
        return 0;
    case TIOCSPGRP: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        int pgrp = 0;
        if (copy_from_user(&pgrp, (const void *)(uintptr_t)arg, sizeof(int)) != 0)
            return -(s64)EFAULT;
        pty->pgrp = (u32)pgrp;
        return 0;
    }
    case TIOCGPGRP: {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        process_t *proc = sched_current_process();
        int pgid = pty->pgrp ? (int)pty->pgrp : (proc ? (int)proc->pgid : 1);
        if (copy_to_user((void *)(uintptr_t)arg, &pgid, sizeof(int)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    case TIOCGSID: {
        process_t *proc = sched_current_process();
        int sid = proc ? (int)proc->pid : 1;
        if (copy_to_user((void *)(uintptr_t)arg, &sid, sizeof(int)) != 0)
            return -(s64)EFAULT;
        return 0;
    }
    default:
        return -(s64)ENOTTY;
    }
}

static file_operations_t g_pts_fops = {
    .open = pts_open,
    .read = pts_read,
    .write = pts_write,
    .ioctl = pts_ioctl,
    .release = pts_release,
};

file_operations_t *pty_get_slave_fops(void)
{
    return &g_pts_fops;
}

file_operations_t *pty_get_ptmx_fops(void)
{
    return &g_ptmx_fops;
}

void pty_init(void)
{
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        g_pty_pairs[i].id = i;
        g_pty_pairs[i].allocated = false;
        g_pty_pairs[i].locked = true;
        pty_init_termios(&g_pty_pairs[i]);
    }

    devfs_register_device("ptmx", &g_ptmx_fops, NULL);
    pr_debug("[PTY] UNIX98 PTY Multiplexer initialized (/dev/ptmx).\n");
}
