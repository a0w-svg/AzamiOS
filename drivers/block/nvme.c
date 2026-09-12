/* ============================================================================
 * AzamiOS — NVM Express (NVMe) 1.x Block Device Driver
 * File: drivers/block/nvme.c
 *
 * A polled (no-IRQ) NVMe driver. For every controller found behind a PCI
 * class-0x01 subclass-0x08 prog-if-0x02 function it:
 *
 *   1. resets the controller (CC.EN=0) and waits for CSTS.RDY to clear,
 *   2. allocates an admin submission/completion queue pair and points the
 *      controller at them via AQA/ASQ/ACQ,
 *   3. re-enables the controller and waits for CSTS.RDY,
 *   4. runs Identify Controller, negotiates one I/O queue pair via
 *      Set Features (Number of Queues), and creates that pair,
 *   5. runs Identify Namespace for each active namespace to learn its block
 *      count and LBA size, and registers a block_dev_t named "nvme0", ….
 *
 * The design deliberately mirrors the in-tree AHCI driver, for the same
 * reasons:
 *   - One command in flight per controller, serialised by a spinlock, so the
 *     queues never need to track more than a single outstanding command.
 *   - Every hardware wait is bounded; a wedged controller yields -EIO rather
 *     than spinning forever with a lock held and interrupts off.
 *   - All data transfer goes through a physically-contiguous 64 KiB bounce
 *     buffer, so a caller's buffer never has to be DMA-safe or page-aligned.
 *     This also makes PRP construction trivial: the buffer is contiguous, so
 *     the PRP list is just consecutive page addresses.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>

#include "nvme.h"
#include "block.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/types.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../hal/pci.h"
#include "../../hal/device.h"
#include "../base/pci_bus.h"

/* ── Controller register offsets (NVMe 1.4 §3.1) ─────────────────────────── */
#define NVME_REG_CAP      0x00   /* u64 Controller Capabilities   */
#define NVME_REG_VS       0x08   /* u32 Version                   */
#define NVME_REG_INTMS    0x0C   /* u32 Interrupt Mask Set        */
#define NVME_REG_INTMC    0x10   /* u32 Interrupt Mask Clear      */
#define NVME_REG_CC       0x14   /* u32 Controller Configuration  */
#define NVME_REG_CSTS     0x1C   /* u32 Controller Status         */
#define NVME_REG_AQA      0x24   /* u32 Admin Queue Attributes    */
#define NVME_REG_ASQ      0x28   /* u64 Admin SQ Base Address     */
#define NVME_REG_ACQ      0x30   /* u64 Admin CQ Base Address     */
#define NVME_REG_DBS      0x1000 /* Doorbell registers start here */

#define NVME_CC_EN        (1U << 0)
#define NVME_CSTS_RDY     (1U << 0)
#define NVME_CSTS_CFS     (1U << 1)   /* Controller Fatal Status */

/* Admin opcodes */
#define NVME_ADM_CREATE_SQ   0x01
#define NVME_ADM_CREATE_CQ   0x05
#define NVME_ADM_IDENTIFY    0x06
#define NVME_ADM_SET_FEAT    0x09

/* NVM I/O opcodes */
#define NVME_CMD_WRITE       0x01
#define NVME_CMD_READ        0x02

#define NVME_FEAT_NUM_QUEUES 0x07

/* Identify CNS values */
#define NVME_CNS_NAMESPACE   0x00
#define NVME_CNS_CONTROLLER  0x01

/* Queue sizing. 64 entries is far more than a one-command-in-flight driver
 * needs, but it keeps both queues comfortably inside a single 4 KiB page
 * (64 * 64 B SQ = 4 KiB; 64 * 16 B CQ = 1 KiB). */
#define NVME_QUEUE_DEPTH     64
#define NVME_ADMIN_QID       0
#define NVME_IO_QID          1

#define NVME_BOUNCE_BYTES    (64 * 1024)
#define NVME_BOUNCE_PAGES    (NVME_BOUNCE_BYTES / PAGE_SIZE)

#define NVME_MAX_CONTROLLERS 8
#define NVME_MAX_NAMESPACES  4

/* Bounded-wait budgets; each iteration is one cpu_pause(). */
#define WAIT_SHORT           2000000u
#define WAIT_LONG            20000000u

/* ── Queue entry layouts (NVMe 1.4 §4.2, §4.6) ───────────────────────────── */
typedef struct {
    u8  opcode;
    u8  flags;
    u16 cid;
    u32 nsid;
    u64 rsvd2;
    u64 mptr;
    u64 prp1;
    u64 prp2;
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
    u32 result;
    u32 rsvd;
    u16 sq_head;
    u16 sq_id;
    u16 cid;
    u16 status;      /* bit 0 = phase tag, bits 15:1 = status field */
} __attribute__((packed)) nvme_cqe_t;

typedef struct nvme_queue {
    nvme_sqe_t     *sq;          /* submission queue (virtual)  */
    nvme_cqe_t     *cq;          /* completion queue (virtual)  */
    phys_addr_t     sq_phys;
    phys_addr_t     cq_phys;
    volatile u32   *sq_db;       /* submission doorbell         */
    volatile u32   *cq_db;       /* completion doorbell         */
    u16             sq_tail;
    u16             cq_head;
    u8              phase;       /* expected phase tag          */
} nvme_queue_t;

typedef struct nvme_ctrl {
    virt_addr_t   regs;          /* mapped BAR0                              */
    u32           dstrd;         /* doorbell stride exponent from CAP        */
    nvme_queue_t  admin;
    nvme_queue_t  io;
    phys_addr_t   bounce_phys;   /* 64 KiB contiguous DMA bounce buffer      */
    void         *bounce_virt;
    phys_addr_t   prp_phys;      /* one page holding the PRP list            */
    u64          *prp_virt;
    phys_addr_t   ident_phys;    /* 4 KiB scratch for Identify payloads      */
    void         *ident_virt;
    spinlock_t    lock;
    u16           next_cid;
} nvme_ctrl_t;

typedef struct nvme_ns {
    nvme_ctrl_t *ctrl;
    u32          nsid;
    u32          lba_bytes;
    block_dev_t  bdev;
} nvme_ns_t;

static nvme_ctrl_t g_ctrls[NVME_MAX_CONTROLLERS];
static u32         g_ctrl_count;
static nvme_ns_t   g_namespaces[NVME_MAX_CONTROLLERS * NVME_MAX_NAMESPACES];
static u32         g_ns_count;

/* ── MMIO accessors ──────────────────────────────────────────────────────── */
static inline u32 reg_r32(nvme_ctrl_t *c, u32 off)
{
    u32 v = *(volatile u32 *)(c->regs + off);
    rmb();
    return v;
}
static inline void reg_w32(nvme_ctrl_t *c, u32 off, u32 v)
{
    wmb();
    *(volatile u32 *)(c->regs + off) = v;
}
static inline u64 reg_r64(nvme_ctrl_t *c, u32 off)
{
    u64 v = *(volatile u64 *)(c->regs + off);
    rmb();
    return v;
}
static inline void reg_w64(nvme_ctrl_t *c, u32 off, u64 v)
{
    wmb();
    *(volatile u64 *)(c->regs + off) = v;
}

/* Wait until (reg & mask) == want, or the budget expires. 0 on success.
 * Also bails out early on CSTS.CFS: once the controller reports fatal status
 * it will never satisfy the condition, and spinning out the full budget just
 * delays the error by several seconds. */
static int wait_csts(nvme_ctrl_t *c, u32 mask, u32 want, u32 budget)
{
    for (u32 i = 0; i < budget; i++) {
        u32 v = reg_r32(c, NVME_REG_CSTS);
        if ((v & mask) == want) return 0;
        if (v & NVME_CSTS_CFS) return -1;
        cpu_pause();
    }
    return -1;
}

/* ── Queue helpers ───────────────────────────────────────────────────────── */

/* Doorbell addresses: base 0x1000, stride (4 << CAP.DSTRD) bytes, ordered
 * SQ0TDBL, CQ0HDBL, SQ1TDBL, CQ1HDBL, … (NVMe 1.4 §3.1.24). */
static void queue_set_doorbells(nvme_ctrl_t *c, nvme_queue_t *q, u32 qid)
{
    u32 stride = 4U << c->dstrd;
    c->regs += 0;   /* no-op; keeps the expression below readable */
    q->sq_db = (volatile u32 *)(c->regs + NVME_REG_DBS + (2 * qid) * stride);
    q->cq_db = (volatile u32 *)(c->regs + NVME_REG_DBS + (2 * qid + 1) * stride);
}

/* Allocate one page for the SQ and one for the CQ. Both must be physically
 * contiguous and page aligned, which pmm_alloc_page() guarantees. */
static int queue_alloc(nvme_queue_t *q)
{
    q->sq_phys = pmm_alloc_page();
    if (!q->sq_phys) return -ENOMEM;
    q->cq_phys = pmm_alloc_page();
    if (!q->cq_phys) { pmm_free_page(q->sq_phys); q->sq_phys = 0; return -ENOMEM; }

    q->sq = (nvme_sqe_t *)PHYS_TO_VIRT(q->sq_phys);
    q->cq = (nvme_cqe_t *)PHYS_TO_VIRT(q->cq_phys);
    __builtin_memset(q->sq, 0, PAGE_SIZE);
    __builtin_memset(q->cq, 0, PAGE_SIZE);

    q->sq_tail = 0;
    q->cq_head = 0;
    q->phase   = 1;   /* controller writes phase 1 on the first pass */
    return 0;
}

static void queue_free(nvme_queue_t *q)
{
    if (q->sq_phys) { pmm_free_page(q->sq_phys); q->sq_phys = 0; }
    if (q->cq_phys) { pmm_free_page(q->cq_phys); q->cq_phys = 0; }
    q->sq = NULL;
    q->cq = NULL;
}

/* Submit `cmd` on `q` and poll its completion queue until the matching CQE
 * appears. Returns the NVMe status field (0 = success) or -1 on timeout.
 * The caller holds ctrl->lock, so exactly one command is ever in flight. */
static int queue_submit_sync(nvme_ctrl_t *c, nvme_queue_t *q, nvme_sqe_t *cmd, u32 *out_result)
{
    u16 cid = c->next_cid++;
    cmd->cid = cid;

    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (u16)((q->sq_tail + 1) % NVME_QUEUE_DEPTH);
    wmb();
    *q->sq_db = q->sq_tail;

    /* Poll for a CQE whose phase tag has flipped to the value we expect. */
    for (u32 i = 0; i < WAIT_LONG; i++) {
        volatile nvme_cqe_t *e = &q->cq[q->cq_head];
        u16 status = e->status;
        rmb();
        if ((status & 1) == q->phase) {
            u32 result = e->result;
            u16 got_cid = e->cid;

            q->cq_head = (u16)((q->cq_head + 1) % NVME_QUEUE_DEPTH);
            if (q->cq_head == 0) q->phase ^= 1;   /* wrapped: phase inverts */
            wmb();
            *q->cq_db = q->cq_head;

            if (got_cid != cid) {
                pr_debug("[NVME] completion cid mismatch (got %u want %u)\n",
                         got_cid, cid);
                return -1;
            }
            if (out_result) *out_result = result;
            return (int)(status >> 1);   /* status field, 0 == success */
        }
        cpu_pause();
    }
    pr_debug("[NVME] command 0x%02x timed out\n", cmd->opcode);
    return -1;
}

/* ── Data transfer ───────────────────────────────────────────────────────── */

/* Fill PRP1/PRP2 for a transfer of `bytes` starting at the (page-aligned,
 * physically contiguous) bounce buffer.
 *
 * PRP rules (NVMe 1.4 §4.3): PRP1 is the first page. If the transfer fits in
 * one page, PRP2 is unused; if it spans exactly two, PRP2 is the second page
 * directly; beyond that PRP2 points at a list of the remaining page addresses.
 * Because the bounce buffer is contiguous the list is just successive pages. */
static void nvme_setup_prp(nvme_ctrl_t *c, nvme_sqe_t *cmd, u32 bytes)
{
    cmd->prp1 = c->bounce_phys;

    if (bytes <= PAGE_SIZE) {
        cmd->prp2 = 0;
    } else if (bytes <= 2 * PAGE_SIZE) {
        cmd->prp2 = c->bounce_phys + PAGE_SIZE;
    } else {
        u32 pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        for (u32 i = 1; i < pages; i++)
            c->prp_virt[i - 1] = c->bounce_phys + (u64)i * PAGE_SIZE;
        wmb();
        cmd->prp2 = c->prp_phys;
    }
}

/* One read or write of at most NVME_BOUNCE_BYTES, via the bounce buffer. */
static s64 nvme_rw_chunk(nvme_ns_t *ns, u64 lba, u32 count, void *buf, bool write)
{
    nvme_ctrl_t *c = ns->ctrl;
    u32 bytes = count * ns->lba_bytes;

    irqflags_t fl = spinlock_lock_irqsave(&c->lock);

    if (write)
        __builtin_memcpy(c->bounce_virt, buf, bytes);

    nvme_sqe_t cmd;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = write ? NVME_CMD_WRITE : NVME_CMD_READ;
    cmd.nsid   = ns->nsid;
    cmd.cdw10  = (u32)(lba & 0xFFFFFFFFu);
    cmd.cdw11  = (u32)(lba >> 32);
    cmd.cdw12  = count - 1;          /* NLB is zero-based */
    nvme_setup_prp(c, &cmd, bytes);

    int st = queue_submit_sync(c, &c->io, &cmd, NULL);

    if (st == 0 && !write)
        __builtin_memcpy(buf, c->bounce_virt, bytes);

    spinlock_unlock_irqrestore(&c->lock, fl);

    if (st != 0) {
        pr_debug("[NVME] %s lba=%llu count=%u failed (status 0x%x)\n",
                 write ? "write" : "read", (unsigned long long)lba, count, st);
        return -EIO;
    }
    return (s64)count;
}

/* Split an arbitrary request into bounce-buffer-sized chunks. */
static s64 nvme_transfer(nvme_ns_t *ns, u64 lba, u32 count, void *buf, bool write)
{
    if (!ns || !buf || count == 0) return -EINVAL;
    if (lba + count > ns->bdev.sector_count) return -EINVAL;

    u32 per_chunk = NVME_BOUNCE_BYTES / ns->lba_bytes;
    if (per_chunk == 0) return -EINVAL;

    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > per_chunk) chunk = per_chunk;
        s64 rc = nvme_rw_chunk(ns, lba + done, chunk,
                               (u8 *)buf + (u64)done * ns->lba_bytes, write);
        if (rc < 0) return done ? (s64)done : rc;
        done += chunk;
    }
    return (s64)done;
}

static s64 nvme_read_sectors(block_dev_t *dev, u64 lba, u32 count, void *buf)
{
    return nvme_transfer((nvme_ns_t *)dev->driver_data, lba, count, buf, false);
}

static s64 nvme_write_sectors(block_dev_t *dev, u64 lba, u32 count, const void *buf)
{
    return nvme_transfer((nvme_ns_t *)dev->driver_data, lba, count, (void *)buf, true);
}

static block_ops_t g_nvme_ops = {
    .read_sectors  = nvme_read_sectors,
    .write_sectors = nvme_write_sectors,
};

/* ── Admin commands ──────────────────────────────────────────────────────── */

static int nvme_identify(nvme_ctrl_t *c, u32 nsid, u32 cns)
{
    nvme_sqe_t cmd;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADM_IDENTIFY;
    cmd.nsid   = nsid;
    cmd.prp1   = c->ident_phys;
    cmd.cdw10  = cns;
    __builtin_memset(c->ident_virt, 0, PAGE_SIZE);
    return queue_submit_sync(c, &c->admin, &cmd, NULL);
}

static int nvme_create_io_queues(nvme_ctrl_t *c)
{
    /* Ask for one I/O queue pair. CDW11 encodes (NSQR | NCQR << 16), both
     * zero-based, so 0 means "one queue". The controller may grant fewer than
     * requested — with one requested there is nothing to negotiate down to,
     * so a success here is enough. */
    nvme_sqe_t cmd;
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADM_SET_FEAT;
    cmd.cdw10  = NVME_FEAT_NUM_QUEUES;
    cmd.cdw11  = 0 | (0 << 16);
    u32 granted = 0;
    if (queue_submit_sync(c, &c->admin, &cmd, &granted) != 0) {
        pr_debug("[NVME] Set Features (Number of Queues) failed\n");
        return -EIO;
    }

    if (queue_alloc(&c->io) != 0) return -ENOMEM;
    queue_set_doorbells(c, &c->io, NVME_IO_QID);

    /* Completion queue first: Create I/O SQ names the CQ it reports into, so
     * the CQ has to exist by then. PC=1 (physically contiguous), IEN=0 since
     * this driver polls rather than taking interrupts. */
    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADM_CREATE_CQ;
    cmd.prp1   = c->io.cq_phys;
    cmd.cdw10  = NVME_IO_QID | ((NVME_QUEUE_DEPTH - 1) << 16);
    cmd.cdw11  = 1;                      /* PC=1, IEN=0 */
    if (queue_submit_sync(c, &c->admin, &cmd, NULL) != 0) {
        pr_debug("[NVME] Create I/O CQ failed\n");
        queue_free(&c->io);
        return -EIO;
    }

    __builtin_memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADM_CREATE_SQ;
    cmd.prp1   = c->io.sq_phys;
    cmd.cdw10  = NVME_IO_QID | ((NVME_QUEUE_DEPTH - 1) << 16);
    cmd.cdw11  = 1 | (NVME_IO_QID << 16); /* PC=1, CQID = our CQ */
    if (queue_submit_sync(c, &c->admin, &cmd, NULL) != 0) {
        pr_debug("[NVME] Create I/O SQ failed\n");
        queue_free(&c->io);
        return -EIO;
    }
    return 0;
}

/* Register one block device per active namespace. */
static void nvme_scan_namespaces(nvme_ctrl_t *c, u32 nn)
{
    if (nn > NVME_MAX_NAMESPACES) nn = NVME_MAX_NAMESPACES;

    for (u32 nsid = 1; nsid <= nn; nsid++) {
        if (g_ns_count >= (u32)(sizeof(g_namespaces) / sizeof(g_namespaces[0]))) return;

        if (nvme_identify(c, nsid, NVME_CNS_NAMESPACE) != 0) continue;

        const u8 *idns = (const u8 *)c->ident_virt;
        u64 nsze = *(const u64 *)(idns + 0);        /* namespace size, blocks */
        u8  flbas = idns[26];                        /* formatted LBA size    */
        if (nsze == 0) continue;                     /* inactive namespace    */

        /* LBA Format table starts at byte 128, 4 bytes per entry; the LBADS
         * field (byte 2 of the entry) is log2 of the block size. */
        u32 fmt_idx = flbas & 0xF;
        u8  lbads   = idns[128 + fmt_idx * 4 + 2];
        if (lbads < 9 || lbads > 12) {
            pr_debug("[NVME]  nsid %u: unsupported LBA size 2^%u — skipped\n", nsid, lbads);
            continue;
        }
        u32 lba_bytes = 1U << lbads;

        nvme_ns_t *ns = &g_namespaces[g_ns_count];
        ns->ctrl      = c;
        ns->nsid      = nsid;
        ns->lba_bytes = lba_bytes;

        __builtin_memset(&ns->bdev, 0, sizeof(ns->bdev));
        ns->bdev.sector_size  = lba_bytes;
        ns->bdev.sector_count = nsze;
        ns->bdev.ops          = &g_nvme_ops;
        ns->bdev.driver_data  = ns;
        /* Linux names these nvme0n1, nvme0n2, …; the block layer here uses a
         * flat name space, so number them sequentially as nvme0, nvme1, …. */
        ns->bdev.name[0] = 'n'; ns->bdev.name[1] = 'v';
        ns->bdev.name[2] = 'm'; ns->bdev.name[3] = 'e';
        ns->bdev.name[4] = (char)('0' + (g_ns_count % 10));
        ns->bdev.name[5] = '\0';

        if (block_dev_register(&ns->bdev) < 0) {
            pr_debug("[NVME]  failed to register %s\n", ns->bdev.name);
            continue;
        }

        u64 mib = (nsze * lba_bytes) / (1024 * 1024);
        pr_debug("[NVME]  nsid %u: %llu blocks x %u B (%llu MiB) -> /dev/%s\n",
                 nsid, (unsigned long long)nsze, lba_bytes,
                 (unsigned long long)mib, ns->bdev.name);
        g_ns_count++;

        /* TEMP-SELFTEST */
        {
            static u8 wbuf[20480], rbuf[20480];
            for (u32 i = 0; i < sizeof(wbuf); i++) wbuf[i] = (u8)(i * 7 + 3);
            u32 nsec = sizeof(wbuf) / lba_bytes;
            s64 w = nvme_write_sectors(&ns->bdev, 100, nsec, wbuf);
            __builtin_memset(rbuf, 0, sizeof(rbuf));
            s64 rr = nvme_read_sectors(&ns->bdev, 100, nsec, rbuf);
            int bad = -1;
            for (u32 i = 0; i < sizeof(wbuf); i++)
                if (wbuf[i] != rbuf[i]) { bad = (int)i; break; }
            pr_debug("[NVME-SELFTEST] %u sectors (%u B, PRP-list path): w=%lld r=%lld mismatch@%d -> %s\n",
                     nsec, (u32)sizeof(wbuf), (long long)w, (long long)rr, bad,
                     (w == (s64)nsec && rr == (s64)nsec && bad < 0) ? "PASS" : "FAIL");

            /* Single-page path (PRP1 only) and two-page path (PRP1+PRP2). */
            s64 w1 = nvme_write_sectors(&ns->bdev, 50, 1, wbuf);
            __builtin_memset(rbuf, 0, 512);
            s64 r1 = nvme_read_sectors(&ns->bdev, 50, 1, rbuf);
            int bad1 = -1;
            for (u32 i = 0; i < 512; i++) if (wbuf[i] != rbuf[i]) { bad1 = (int)i; break; }
            pr_debug("[NVME-SELFTEST] 1 sector (PRP1 only): w=%lld r=%lld mismatch@%d -> %s\n",
                     (long long)w1, (long long)r1, bad1,
                     (w1 == 1 && r1 == 1 && bad1 < 0) ? "PASS" : "FAIL");

            s64 w2 = nvme_write_sectors(&ns->bdev, 60, 16, wbuf);
            __builtin_memset(rbuf, 0, 8192);
            s64 r2 = nvme_read_sectors(&ns->bdev, 60, 16, rbuf);
            int bad2 = -1;
            for (u32 i = 0; i < 8192; i++) if (wbuf[i] != rbuf[i]) { bad2 = (int)i; break; }
            pr_debug("[NVME-SELFTEST] 16 sectors (PRP1+PRP2): w=%lld r=%lld mismatch@%d -> %s\n",
                     (long long)w2, (long long)r2, bad2,
                     (w2 == 16 && r2 == 16 && bad2 < 0) ? "PASS" : "FAIL");
        }
    }
}

/* ── Controller bring-up ─────────────────────────────────────────────────── */

static void nvme_controller_init(device_t *dev, pci_device_info_t *pci)
{
    if (g_ctrl_count >= NVME_MAX_CONTROLLERS) return;

    pr_debug("[NVME] controller %02x:%02x.%x (%04x:%04x)\n",
             pci->bus, pci->slot, pci->func, pci->vendor_id, pci->device_id);

    pci_enable_bus_mastering(dev);

    phys_addr_t bar_phys = pci_get_bar(dev, 0);
    if (!bar_phys) { pr_debug("[NVME] no BAR0\n"); return; }

    nvme_ctrl_t *c = &g_ctrls[g_ctrl_count];
    __builtin_memset(c, 0, sizeof(*c));
    spinlock_init(&c->lock);

    /* Map the register window. 8 KiB covers the fixed registers plus the
     * doorbell page for the couple of queues this driver creates. */
    phys_addr_t bar_pg = ALIGN_DOWN(bar_phys, PAGE_SIZE);
    virt_addr_t bar_va = (virt_addr_t)PHYS_TO_VIRT(bar_pg);
    for (u32 off = 0; off < 0x2000; off += PAGE_SIZE)
        vmm_map(0, bar_va + off, bar_pg + off, VMM_MMIO);
    c->regs = bar_va + (bar_phys - bar_pg);

    u64 cap = reg_r64(c, NVME_REG_CAP);
    c->dstrd = (u32)((cap >> 32) & 0xF);
    u32 mqes = (u32)(cap & 0xFFFF) + 1;          /* max queue entries         */
    u32 to   = (u32)((cap >> 24) & 0xFF);        /* timeout, 500 ms units     */
    u32 vs   = reg_r32(c, NVME_REG_VS);

    pr_debug("[NVME]  version %u.%u.%u  MQES=%u  DSTRD=%u  TO=%u00ms\n",
             (vs >> 16) & 0xFFFF, (vs >> 8) & 0xFF, vs & 0xFF, mqes, c->dstrd, to * 5);

    if (mqes < NVME_QUEUE_DEPTH) {
        pr_debug("[NVME]  controller queue limit %u below required %u — skipped\n",
                 mqes, NVME_QUEUE_DEPTH);
        return;
    }

    /* Reset: clear CC.EN and wait for the controller to report not-ready. */
    reg_w32(c, NVME_REG_CC, 0);
    if (wait_csts(c, NVME_CSTS_RDY, 0, WAIT_LONG) != 0) {
        pr_debug("[NVME]  controller failed to go not-ready\n");
        return;
    }

    if (queue_alloc(&c->admin) != 0) {
        pr_debug("[NVME]  admin queue allocation failed\n");
        return;
    }
    queue_set_doorbells(c, &c->admin, NVME_ADMIN_QID);

    /* AQA holds both admin queue sizes as zero-based counts. */
    reg_w32(c, NVME_REG_AQA, (NVME_QUEUE_DEPTH - 1) | ((NVME_QUEUE_DEPTH - 1) << 16));
    reg_w64(c, NVME_REG_ASQ, c->admin.sq_phys);
    reg_w64(c, NVME_REG_ACQ, c->admin.cq_phys);

    /* CC: MPS=0 (4 KiB pages), CSS=0 (NVM command set), IOSQES=6 (64 B SQ
     * entries), IOCQES=4 (16 B CQ entries), then enable. */
    u32 cc = NVME_CC_EN | (0U << 7) | (0U << 11) | (6U << 16) | (4U << 20);
    reg_w32(c, NVME_REG_CC, cc);

    if (wait_csts(c, NVME_CSTS_RDY, NVME_CSTS_RDY, WAIT_LONG) != 0) {
        pr_debug("[NVME]  controller failed to become ready\n");
        queue_free(&c->admin);
        return;
    }

    /* DMA scratch: Identify payload page, PRP list page, and the bounce
     * buffer. Allocated once per controller and never freed — the controller
     * lives for the lifetime of the system. */
    c->ident_phys = pmm_alloc_page();
    c->prp_phys   = pmm_alloc_page();
    c->bounce_phys = pmm_alloc_pages(NVME_BOUNCE_PAGES);
    if (!c->ident_phys || !c->prp_phys || !c->bounce_phys) {
        pr_debug("[NVME]  DMA buffer allocation failed\n");
        if (c->ident_phys)  pmm_free_page(c->ident_phys);
        if (c->prp_phys)    pmm_free_page(c->prp_phys);
        if (c->bounce_phys) pmm_free_pages(c->bounce_phys, NVME_BOUNCE_PAGES);
        queue_free(&c->admin);
        return;
    }
    c->ident_virt  = (void *)PHYS_TO_VIRT(c->ident_phys);
    c->prp_virt    = (u64 *)PHYS_TO_VIRT(c->prp_phys);
    c->bounce_virt = (void *)PHYS_TO_VIRT(c->bounce_phys);

    if (nvme_identify(c, 0, NVME_CNS_CONTROLLER) != 0) {
        pr_debug("[NVME]  Identify Controller failed\n");
        return;
    }

    /* Identify Controller: model string at byte 24 (40 chars, space padded),
     * number of namespaces at byte 516. */
    const u8 *idc = (const u8 *)c->ident_virt;
    char model[41];
    for (int i = 0; i < 40; i++) model[i] = (char)idc[24 + i];
    model[40] = '\0';
    for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
    u32 nn = *(const u32 *)(idc + 516);

    pr_debug("[NVME]  model '%s', %u namespace(s)\n", model, nn);

    if (nvme_create_io_queues(c) != 0) return;

    g_ctrl_count++;             /* commit only once the controller is usable */
    nvme_scan_namespaces(c, nn);
}

static int nvme_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci) return -ENODEV;
    /* nvme_controller_init() is "best effort" by design (see its many bare
     * early returns above) and reports its own failures via pr_debug, so
     * there is nothing more specific to hand back here. */
    nvme_controller_init(dm->hal, pci);
    return 0;
}

static void nvme_remove(dm_device_t *dm)
{
    (void)dm;
    /* Controllers are brought up once and live for the lifetime of the
     * system, same as AHCI — nothing to unwind on a re-probe. */
}

/* Class 0x01 (mass storage), subclass 0x08 (NVMe), prog-if 0x02 (NVM Express
 * I/O controller). One driver instance binds every matching controller the
 * bus finds. */
static const pci_device_id_t nvme_pci_ids[] = {
    { PCI_DEVICE_CLASS(0x010802, 0xFFFFFF) },
    { 0 }
};

static pci_driver_t nvme_pci_driver = {
    .drv      = { .name = "nvme" },
    .id_table = nvme_pci_ids,
    .probe    = nvme_probe,
    .remove   = nvme_remove,
};

/* Registers the PCI driver; probe() binds to every matching NVMe controller
 * the bus already enumerated, so this is safe to call whether or not the
 * host has one. */
void block_nvme_init(void)
{
    g_ctrl_count = 0;
    g_ns_count   = 0;
    pci_driver_register(&nvme_pci_driver);
    if (g_ns_count == 0)
        pr_debug("[NVME] no NVMe namespaces found\n");
}
