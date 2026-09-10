/* ============================================================================
 * AzamiOS — virtio-balloon: cooperative memory reclaim
 * File: drivers/misc/virtio_balloon.c
 *
 * The host asks for memory back by writing a target into the device config;
 * the guest answers by allocating that many pages, handing their frame
 * numbers to the device through the inflate queue, and reporting the new
 * balloon size in `actual`.  Shrinking the balloon runs the same exchange in
 * reverse over the deflate queue, and the pages go back to the allocator.
 *
 * The target is a level, not a delta, so the driver only has to notice when
 * it changes.  A kernel thread polls the config once a second rather than
 * taking the device's configuration-change interrupt, which keeps it off the
 * shared PCI INTx line — the same choice the other VirtIO drivers here make.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "../base/pci_bus.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define VIRTIO_BALLOON_INFLATEQ   0
#define VIRTIO_BALLOON_DEFLATEQ   1

/* The device counts in 4 KiB units regardless of the host's page size. */
#define BALLOON_PFN_SHIFT         12
#define BALLOON_BATCH             256    /* PFNs handed over per transaction */
#define BALLOON_MAX_PAGES         (256 * 1024)   /* 1 GiB ceiling */

/* Device configuration space. */
struct virtio_balloon_config {
    u32 num_pages;   /* target, in 4 KiB units — written by the host  */
    u32 actual;      /* current balloon size — written by the driver  */
} __packed;

typedef struct balloon_device {
    virtio_pci_device_t vpci;
    virtqueue_t *inflate_vq;
    virtqueue_t *deflate_vq;

    u32  *pfn_batch;              /* the array handed to the device      */
    phys_addr_t *pages;           /* frames currently held by the balloon */
    u32   npages;
    u32   target;
    spinlock_t lock;
    bool  ready;
} balloon_device_t;

static balloon_device_t g_balloon;

/* ── Config space ────────────────────────────────────────────────────────── */

static u32 balloon_read_target(balloon_device_t *b)
{
    volatile u8 *cfg = b->vpci.device_cfg;
    if (!cfg) return 0;
    u32 v = 0;
    for (u32 i = 0; i < 4; i++) v |= (u32)cfg[i] << (i * 8);
    return v;
}

static void balloon_write_actual(balloon_device_t *b, u32 actual)
{
    volatile u8 *cfg = b->vpci.device_cfg;
    if (!cfg) return;
    for (u32 i = 0; i < 4; i++) cfg[4 + i] = (u8)(actual >> (i * 8));
}

/* Hand one batch of frame numbers to the device and wait for it to be used. */
static void balloon_tell_host(balloon_device_t *b, virtqueue_t *vq, u16 qindex, u32 count)
{
    if (count == 0) return;

    phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)b->pfn_batch);
    if (virtqueue_add_buf(vq, phys, count * sizeof(u32), false, (void *)1) != 0) return;

    virtqueue_kick(vq);
    virtio_pci_notify(&b->vpci, qindex, vq);

    /* Bounded: a host that never completes the request must not wedge the
     * thread, and the pages stay accounted for either way. */
    for (u32 spins = 0; spins < 1000000; spins++) {
        u32 len = 0;
        if (virtqueue_get_used(vq, &len) != NULL) return;
        cpu_pause();
    }
}

/* ── Inflate / deflate ───────────────────────────────────────────────────── */

static void balloon_inflate(balloon_device_t *b, u32 want)
{
    while (want > 0) {
        u32 batch = want < BALLOON_BATCH ? want : BALLOON_BATCH;
        u32 got = 0;

        for (u32 i = 0; i < batch; i++) {
            if (b->npages >= BALLOON_MAX_PAGES) break;
            phys_addr_t page = pmm_alloc_page();
            if (!page) break;                 /* out of memory: stop here */
            b->pages[b->npages++] = page;
            b->pfn_batch[got++] = (u32)(page >> BALLOON_PFN_SHIFT);
        }
        if (got == 0) break;

        balloon_tell_host(b, b->inflate_vq, VIRTIO_BALLOON_INFLATEQ, got);
        want -= got;
        if (got < batch) break;
    }
}

static void balloon_deflate(balloon_device_t *b, u32 want)
{
    while (want > 0 && b->npages > 0) {
        u32 batch = want < BALLOON_BATCH ? want : BALLOON_BATCH;
        if (batch > b->npages) batch = b->npages;

        for (u32 i = 0; i < batch; i++) {
            b->pfn_batch[i] = (u32)(b->pages[b->npages - 1 - i] >> BALLOON_PFN_SHIFT);
        }

        /* The host must be told before the pages are reused: it may still be
         * treating them as free and discarding writes to them. */
        balloon_tell_host(b, b->deflate_vq, VIRTIO_BALLOON_DEFLATEQ, batch);

        for (u32 i = 0; i < batch; i++) {
            pmm_free_page(b->pages[--b->npages]);
        }
        want -= batch;
    }
}

static void balloon_thread(void *arg)
{
    (void)arg;
    balloon_device_t *b = &g_balloon;

    for (;;) {
        /* Once a second: the balloon is a slow, cooperative mechanism and
         * polling it harder would only burn CPU. */
        sched_sleep(100);
        if (!b->ready) continue;

        u32 target = balloon_read_target(b);
        if (target == b->target) continue;

        spinlock_lock(&b->lock);
        b->target = target;
        if (target > b->npages)      balloon_inflate(b, target - b->npages);
        else if (target < b->npages) balloon_deflate(b, b->npages - target);
        balloon_write_actual(b, b->npages);
        spinlock_unlock(&b->lock);

        pr_debug("[BALLOON] target %u pages, holding %u (%u MiB)\n",
                 target, b->npages, (b->npages * 4) / 1024);
    }
}

/* ── PCI binding ─────────────────────────────────────────────────────────── */

static int balloon_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_balloon.ready) return -EBUSY;

    memset(&g_balloon, 0, sizeof(g_balloon));
    spinlock_init(&g_balloon.lock);
    balloon_device_t *b = &g_balloon;

    if (virtio_pci_init_device(dm->hal, &b->vpci) < 0) return -ENODEV;

    virtio_pci_set_status(&b->vpci, 0);
    virtio_pci_set_status(&b->vpci, VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* No optional features: plain page inflate/deflate is all this needs. */
    if (!virtio_pci_negotiate_features(&b->vpci, 0)) {
        virtio_pci_set_status(&b->vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENODEV;
    }

    b->inflate_vq = virtio_pci_setup_queue(&b->vpci, VIRTIO_BALLOON_INFLATEQ);
    b->deflate_vq = virtio_pci_setup_queue(&b->vpci, VIRTIO_BALLOON_DEFLATEQ);
    if (!b->inflate_vq || !b->deflate_vq) {
        virtio_pci_set_status(&b->vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENODEV;
    }
    b->inflate_vq->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    b->deflate_vq->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    b->pfn_batch = (u32 *)kzalloc(BALLOON_BATCH * sizeof(u32));
    b->pages     = (phys_addr_t *)kzalloc(BALLOON_MAX_PAGES * sizeof(phys_addr_t));
    if (!b->pfn_batch || !b->pages) {
        if (b->pfn_batch) kfree(b->pfn_batch);
        if (b->pages)     kfree(b->pages);
        virtio_pci_set_status(&b->vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENOMEM;
    }

    virtio_pci_set_status(&b->vpci,
                          virtio_pci_get_status(&b->vpci) | VIRTIO_CONFIG_S_DRIVER_OK);

    pci_device_info_t *info = to_pci_info(dm);
    if (info) {
        u16 cmd = pci_config_read16(info->bus, info->slot, info->func, PCI_COMMAND);
        pci_config_write16(info->bus, info->slot, info->func, PCI_COMMAND,
                           (u16)(cmd | PCI_CMD_INTERRUPT_DIS));
    }

    balloon_write_actual(b, 0);
    b->ready = true;
    dm_set_drvdata(dm, b);

    process_t *kproc = sched_kernel_process();
    if (kproc) thread_create(kproc, (uintptr_t)balloon_thread, 0, true);

    pr_debug("[BALLOON] virtio memory balloon ready (target %u pages)\n",
             balloon_read_target(b));
    return 0;
}

static void balloon_remove(dm_device_t *dm)
{
    (void)dm;
    if (!g_balloon.ready) return;

    spinlock_lock(&g_balloon.lock);
    g_balloon.ready = false;
    balloon_deflate(&g_balloon, g_balloon.npages);   /* give it all back */
    spinlock_unlock(&g_balloon.lock);

    virtio_pci_set_status(&g_balloon.vpci, 0);
}

static const pci_device_id_t balloon_pci_ids[] = {
    { PCI_DEVICE(0x1AF4, 0x1045) },   /* virtio-balloon (modern) */
    { PCI_DEVICE(0x1AF4, 0x1002) },   /* virtio-balloon (legacy) */
    { 0 }
};

static pci_driver_t balloon_pci_driver = {
    .drv      = { .name = "virtio_balloon" },
    .id_table = balloon_pci_ids,
    .probe    = balloon_probe,
    .remove   = balloon_remove,
};

void virtio_balloon_init(void)
{
    pci_driver_register(&balloon_pci_driver);
}
