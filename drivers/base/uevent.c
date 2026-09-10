/* ============================================================================
 * AzamiOS — Driver Model Hotplug Events (/dev/kevent)
 * File: drivers/base/uevent.c
 *
 * Linux broadcasts hotplug events over a netlink socket that udev listens on.
 * AzamiOS has no netlink, so the same information is published as a character
 * device: every add/remove/bind/unbind in the driver model appends one
 * NUL-free line to a shared ring, and each open file descriptor walks that
 * ring from wherever it was when the descriptor was opened.
 *
 *   $ cat /dev/kevent
 *   add@/devices/pci/0000:00:02.0 SUBSYSTEM=pci SEQNUM=7 MODALIAS=pci:v00001234d00001111
 *
 * Readers never block: an empty ring returns 0 bytes, and poll() reports
 * POLLIN only while unread events remain, so a hotplug daemon can select()
 * on it alongside its other descriptors.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "base.h"
#include "../../fs/vfs.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#ifndef POLLIN
#define POLLIN      0x0001
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#endif

#define UEVENT_RING_SIZE   64      /* events retained before the oldest ages out */
#define UEVENT_LINE_MAX   192

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

typedef struct {
    char line[UEVENT_LINE_MAX];
    u32  len;
} uevent_slot_t;

static uevent_slot_t g_ring[UEVENT_RING_SIZE];
static u64           g_seq_head;               /* seqnum of the next event  */
static spinlock_t    g_uevent_lock = SPINLOCK_INIT;

/* Per-open reader cursor: the sequence number it has not yet delivered. */
typedef struct {
    u64 next_seq;
} uevent_reader_t;

void dm_uevent(const char *action, dm_device_t *dev)
{
    if (!action || !dev) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_uevent_lock);

    u64 seq  = g_seq_head++;
    uevent_slot_t *slot = &g_ring[seq % UEVENT_RING_SIZE];

    const char *subsys = dev->cls ? dev->cls->name
                       : dev->bus ? dev->bus->name : "device";
    int n = scnprintf(slot->line, sizeof(slot->line),
                     "%s@/devices/%s/%s SUBSYSTEM=%s SEQNUM=%llu MODALIAS=%s\n",
                     action, subsys, dev->name, subsys,
                     (unsigned long long)seq,
                     dev->modalias[0] ? dev->modalias : dev->name);
    slot->len = (n > 0) ? (u32)n : 0;

    spinlock_unlock_irqrestore(&g_uevent_lock, flags);
}

u64 dm_uevent_seqnum(void)
{
    return g_seq_head;
}

/* ── /dev/kevent character device ────────────────────────────────────────── */

static s64 kevent_open(inode_t *inode, file_t *filp)
{
    (void)inode;
    uevent_reader_t *rd = (uevent_reader_t *)kzalloc(sizeof(uevent_reader_t));
    if (!rd) return -(s64)ENOMEM;

    /* Start from the oldest event still in the ring, as udev would after a
     * coldplug: everything already generated is replayed once. */
    irqflags_t flags = spinlock_lock_irqsave(&g_uevent_lock);
    rd->next_seq = (g_seq_head > UEVENT_RING_SIZE) ? g_seq_head - UEVENT_RING_SIZE : 0;
    spinlock_unlock_irqrestore(&g_uevent_lock, flags);

    filp->private_data = rd;
    return 0;
}

static s64 kevent_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static s64 kevent_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf) return -(s64)EINVAL;
    uevent_reader_t *rd = (uevent_reader_t *)filp->private_data;

    size_t written = 0;
    char  *out     = (char *)buf;

    irqflags_t flags = spinlock_lock_irqsave(&g_uevent_lock);

    /* A slow reader that fell off the back of the ring resumes at the oldest
     * event still held, rather than replaying overwritten slots. */
    u64 oldest = (g_seq_head > UEVENT_RING_SIZE) ? g_seq_head - UEVENT_RING_SIZE : 0;
    if (rd->next_seq < oldest) rd->next_seq = oldest;

    while (rd->next_seq < g_seq_head) {
        uevent_slot_t *slot = &g_ring[rd->next_seq % UEVENT_RING_SIZE];
        if (written + slot->len > len) break;
        memcpy(out + written, slot->line, slot->len);
        written += slot->len;
        rd->next_seq++;
    }

    spinlock_unlock_irqrestore(&g_uevent_lock, flags);
    return (s64)written;
}

static int kevent_poll(file_t *filp)
{
    if (!filp || !filp->private_data) return POLLNVAL;
    uevent_reader_t *rd = (uevent_reader_t *)filp->private_data;
    return (rd->next_seq < g_seq_head) ? (POLLIN | POLLRDNORM) : 0;
}

static file_operations_t g_kevent_fops = {
    .read    = kevent_read,
    .open    = kevent_open,
    .release = kevent_release,
    .poll    = kevent_poll,
};

void uevent_init(void)
{
    devfs_register_device("kevent", &g_kevent_fops, NULL);
    pr_debug("[DEVCORE] hotplug event stream at /dev/kevent (%llu events queued)\n",
             (unsigned long long)g_seq_head);
}
