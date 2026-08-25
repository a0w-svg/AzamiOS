/* ============================================================================
 * AzamiOS — UNIX98 Pseudo-Terminal (PTY) Subsystem Implementation
 * File: drivers/char/pty.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
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
        return 0;
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
    case TCGETS:
    case TCSETS:
        return 0;
    default:
        return 0;
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

    filp->private_data = pty;
    filp->f_op = &g_ptm_fops;
    spinlock_unlock(&g_pty_lock);

    return 0;
}

static file_operations_t g_ptmx_fops = {
    .open = ptmx_open,
};

/* ── Slave operations ────────────────────────────────────────────────────── */

static s64 pts_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    pty_pair_t *pty = (pty_pair_t *)filp->private_data;

    spinlock_lock(&g_pty_lock);
    if (pty->m2s_count == 0) {
        spinlock_unlock(&g_pty_lock);
        return 0;
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
    case TCGETS:
    case TCSETS:
        return 0;
    default:
        return 0;
    }
}

static file_operations_t g_pts_fops = {
    .read = pts_read,
    .write = pts_write,
    .ioctl = pts_ioctl,
};

file_operations_t *pty_get_slave_fops(void)
{
    return &g_pts_fops;
}

void pty_init(void)
{
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        g_pty_pairs[i].id = i;
        g_pty_pairs[i].allocated = false;
        g_pty_pairs[i].locked = true;
    }

    devfs_register_device("ptmx", &g_ptmx_fops, NULL);
    pr_debug("[PTY] UNIX98 PTY Multiplexer initialized (/dev/ptmx).\n");
}
