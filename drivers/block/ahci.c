/* ============================================================================
 * AzamiOS — AHCI 1.x / SATA Block Device Driver
 * File: drivers/block/ahci.c
 *
 * A polled (no-IRQ) AHCI port driver. For every SATA drive found behind a
 * PCI class-0x01 subclass-0x06 controller it:
 *
 *   1. performs the BIOS/OS ownership handoff and an HBA reset,
 *   2. rebases each implemented port onto a private 4 KiB DMA page
 *      (command list + received-FIS area + one command table),
 *   3. issues ATA IDENTIFY DEVICE to learn the real capacity / model, and
 *   4. registers a block_dev_t named "sata0", "sata1", … so the VFS and
 *      /dev/sataN see a correctly-sized disk.
 *
 * Design choices that keep this simple and robust:
 *   - Single command slot (slot 0) per port, serialised by a per-drive lock.
 *   - Every hardware wait loop is bounded; a stuck controller yields -EIO
 *     instead of wedging the kernel with a spinlock held and IRQs off
 *     (the old in-tree implementation had three unbounded `while` loops).
 *   - All data transfer goes through a per-drive 64 KiB physically-contiguous
 *     bounce buffer, so a caller-supplied buffer never has to be DMA-safe,
 *     page-aligned, or even part of the HHDM linear map.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>

#include "ahci.h"
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

/* ── Register bits not already in ahci.h ─────────────────────────────────── */
#define AHCI_GHC_HR         (1U << 0)     /* HBA reset            */
#define AHCI_GHC_IE         (1U << 1)     /* interrupt enable     */
#define AHCI_GHC_AE         (1U << 31)    /* AHCI enable          */
#define AHCI_CAP2_BOH       (1U << 0)     /* BIOS/OS handoff supported */
#define AHCI_BOHC_BOS       (1U << 0)     /* BIOS owned           */
#define AHCI_BOHC_OOS       (1U << 1)     /* OS owned             */
#define AHCI_BOHC_BB        (1U << 4)     /* BIOS busy            */

#define AHCI_PxTFD_BSY      (1U << 7)
#define AHCI_PxTFD_DRQ      (1U << 3)
#define AHCI_PxTFD_ERR      (1U << 0)

#define AHCI_PxIS_ERR_MASK  (HBA_PxIS_TFES | (1U << 29) /* HBFS */ | \
                             (1U << 28) /* HBDS */ | (1U << 27) /* IFS */ | \
                             (1U << 26) /* INFS */)

#define ATA_CMD_IDENTIFY_DEV 0xEC

#define AHCI_MAX_DRIVES      16
#define AHCI_BOUNCE_BYTES    (64 * 1024)          /* 128 sectors per command */
#define AHCI_BOUNCE_PAGES    (AHCI_BOUNCE_BYTES / PAGE_SIZE)

/* Bounded-wait iteration budgets (each iteration does one cpu_pause()). */
#define WAIT_SHORT          2000000u
#define WAIT_LONG           20000000u

typedef struct ahci_drive {
    ahci_port_t *port;          /* MMIO port register block                 */
    u64          page_phys;     /* private CLB/FIS/cmdtbl page              */
    void        *page_virt;
    u64          bounce_phys;   /* 64 KiB contiguous DMA bounce buffer      */
    void        *bounce_virt;
    u32          port_no;
    spinlock_t   lock;
    block_dev_t  bdev;
} ahci_drive_t;

static ahci_drive_t g_drives[AHCI_MAX_DRIVES];
static u32          g_drive_count;

/* ── MMIO accessors (volatile + ordering) ───────────────────────────────── */

static inline u32 mr(volatile u32 *reg)       { u32 v = *reg; rmb(); return v; }
static inline void mw(volatile u32 *reg, u32 v) { wmb(); *reg = v; }

/* ── Bounded waits ──────────────────────────────────────────────────────── */

/* Wait until (*reg & mask) == want, or the budget expires. 0 on success. */
static int wait_bits(volatile u32 *reg, u32 mask, u32 want, u32 budget)
{
    for (u32 i = 0; i < budget; i++) {
        if ((mr(reg) & mask) == want) return 0;
        cpu_pause();
    }
    return -1;
}

/* ── Port start / stop ──────────────────────────────────────────────────── */

static int port_stop(ahci_port_t *p)
{
    u32 cmd = mr(&p->cmd);
    cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE);
    mw(&p->cmd, cmd);
    /* CR and FR must both go clear within 500 ms per spec. */
    if (wait_bits(&p->cmd, HBA_PxCMD_CR | HBA_PxCMD_FR, 0, WAIT_LONG) != 0)
        return -1;
    return 0;
}

static void port_start(ahci_port_t *p)
{
    /* Spec: wait for CR clear before setting ST. */
    wait_bits(&p->cmd, HBA_PxCMD_CR, 0, WAIT_SHORT);
    mw(&p->cmd, mr(&p->cmd) | HBA_PxCMD_FRE);
    mw(&p->cmd, mr(&p->cmd) | HBA_PxCMD_ST);
}

/* ── Issue the command in slot 0 and wait for completion ────────────────── */

static int port_run_slot0(ahci_drive_t *d)
{
    ahci_port_t *p = d->port;

    /* Clear any latched error/interrupt status. */
    mw(&p->serr, 0xFFFFFFFFu);
    mw(&p->is, 0xFFFFFFFFu);

    /* Device must not be BSY/DRQ before we hand it a new command. */
    if (wait_bits(&p->tfd, AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ, 0, WAIT_LONG) != 0)
        return -(s64)EIO;

    mw(&p->ci, 1u);

    for (u32 i = 0; i < WAIT_LONG; i++) {
        u32 ci = mr(&p->ci);
        u32 is = mr(&p->is);
        if (is & AHCI_PxIS_ERR_MASK) {
            mw(&p->is, is);
            mw(&p->serr, 0xFFFFFFFFu);
            return -(s64)EIO;
        }
        if ((ci & 1u) == 0) return 0;
        cpu_pause();
    }
    return -(s64)EIO;   /* timed out — leave the port for the next reset */
}

/* ── Build the slot-0 command (FIS + single-entry PRDT) ─────────────────── */

static void build_cmd(ahci_drive_t *d, u8 ata_cmd, u64 lba, u16 sectors,
                      u32 bytes, int write)
{
    ahci_cmd_header_t *hdr   = (ahci_cmd_header_t *)d->page_virt;
    ahci_cmd_table_t  *tbl   = (ahci_cmd_table_t *)((u8 *)d->page_virt + 0x500);

    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl   = sizeof(fis_reg_h2d_t) / sizeof(u32);   /* FIS length in DWORDs */
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = 1;
    hdr->ctba  = (u32)(d->page_phys + 0x500);
    hdr->ctbau = (u32)((d->page_phys + 0x500) >> 32);

    memset(tbl, 0, sizeof(*tbl));
    tbl->prdt_entry[0].dba  = (u32)d->bounce_phys;
    tbl->prdt_entry[0].dbau = (u32)(d->bounce_phys >> 32);
    tbl->prdt_entry[0].dbc  = (bytes - 1) & 0x3FFFFF;   /* 22-bit byte count, 0-based */

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;                 /* this is a command, not a control FIS */
    fis->command  = ata_cmd;
    fis->device   = 1 << 6;            /* LBA mode */
    fis->lba0 = (u8)lba;   fis->lba1 = (u8)(lba >> 8);  fis->lba2 = (u8)(lba >> 16);
    fis->lba3 = (u8)(lba >> 24); fis->lba4 = (u8)(lba >> 32); fis->lba5 = (u8)(lba >> 40);
    fis->countl = (u8)sectors;
    fis->counth = (u8)(sectors >> 8);
}

/* ── block_ops: read / write ───────────────────────────────────────────── */

static s64 ahci_rw(ahci_drive_t *d, u64 lba, u32 count, void *buf, int write)
{
    if (!buf || count == 0) return -(s64)EINVAL;
    if (lba + count > d->bdev.sector_count) return -(s64)EINVAL;

    irqflags_t f = spinlock_lock_irqsave(&d->lock);

    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > AHCI_BOUNCE_BYTES / 512) chunk = AHCI_BOUNCE_BYTES / 512;
        u32 bytes = chunk * 512;

        if (write)
            memcpy(d->bounce_virt, (u8 *)buf + (u64)done * 512, bytes);

        build_cmd(d, write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX,
                  lba + done, (u16)chunk, bytes, write);

        int rc = port_run_slot0(d);
        if (rc != 0) {
            spinlock_unlock_irqrestore(&d->lock, f);
            return rc;
        }

        if (!write)
            memcpy((u8 *)buf + (u64)done * 512, d->bounce_virt, bytes);

        done += chunk;
    }

    spinlock_unlock_irqrestore(&d->lock, f);
    return (s64)(count * 512);
}

static s64 ahci_read_sectors(block_dev_t *bd, u64 lba, u32 count, void *buf)
{
    return ahci_rw((ahci_drive_t *)bd->driver_data, lba, count, buf, 0);
}

static s64 ahci_write_sectors(block_dev_t *bd, u64 lba, u32 count, const void *buf)
{
    return ahci_rw((ahci_drive_t *)bd->driver_data, lba, count, (void *)buf, 1);
}

static block_ops_t g_ahci_ops = {
    .read_sectors  = ahci_read_sectors,
    .write_sectors = ahci_write_sectors,
};

/* ── IDENTIFY DEVICE → capacity + model ─────────────────────────────────── */

static int ahci_identify(ahci_drive_t *d)
{
    build_cmd(d, ATA_CMD_IDENTIFY_DEV, 0, 1, 512, 0);
    if (port_run_slot0(d) != 0) return -1;

    const u16 *id = (const u16 *)d->bounce_virt;

    u64 sectors;
    if ((id[83] & (1 << 10)) && (id[86] & (1 << 10))) {
        sectors = ((u64)id[103] << 48) | ((u64)id[102] << 32) |
                  ((u64)id[101] << 16) |  (u64)id[100];
    } else {
        sectors = ((u32)id[61] << 16) | id[60];
    }
    if (sectors == 0) return -1;
    d->bdev.sector_count = sectors;

    /* Model string: words 27..46, byte-swapped ASCII. */
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = '\0';
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == '\0'); i--)
        model[i] = '\0';

    pr_debug("[AHCI]  port %u: '%s' — %llu sectors (%llu MiB)\n",
             d->port_no, model, (unsigned long long)sectors,
             (unsigned long long)(sectors / 2048));
    return 0;
}

/* ── Per-port bring-up ─────────────────────────────────────────────────── */

static void ahci_port_bringup(ahci_port_t *port, u32 port_no)
{
    if (g_drive_count >= AHCI_MAX_DRIVES) return;

    /* Wait for SATA PHY link (DET reaches 3). After an HBA reset this takes a
     * few ms of COMRESET/COMWAKE negotiation, so an empty slot must be given
     * that grace period before being written off. */
    u32 ssts = 0;
    for (u32 i = 0; i < WAIT_SHORT; i++) {
        ssts = mr(&port->ssts);
        if ((ssts & 0x0F) == 3) break;
        cpu_pause();
    }
    if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1) return;   /* no device */

    ahci_drive_t *d = &g_drives[g_drive_count];
    memset(d, 0, sizeof(*d));
    d->port    = port;
    d->port_no = port_no;
    d->lock    = (spinlock_t)SPINLOCK_INIT;

    if (port_stop(port) != 0) {
        pr_debug("[AHCI]  port %u: engine would not stop, skipped\n", port_no);
        return;
    }

    d->page_phys = pmm_alloc_page();
    d->bounce_phys = pmm_alloc_pages(AHCI_BOUNCE_PAGES);
    if (!d->page_phys || !d->bounce_phys) {
        if (d->page_phys) pmm_free_page(d->page_phys);
        if (d->bounce_phys) pmm_free_pages(d->bounce_phys, AHCI_BOUNCE_PAGES);
        pr_debug("[AHCI]  port %u: out of memory\n", port_no);
        return;
    }
    d->page_virt   = PHYS_TO_VIRT(d->page_phys);
    d->bounce_virt = PHYS_TO_VIRT(d->bounce_phys);
    memset(d->page_virt, 0, PAGE_SIZE);

    /* Page layout: 0x000 command list (1 KiB) | 0x400 received FIS (256 B) |
     *              0x500 command table 0 (256 B). */
    mw(&port->clb,  (u32)d->page_phys);
    mw(&port->clbu, (u32)(d->page_phys >> 32));
    mw(&port->fb,   (u32)(d->page_phys + 0x400));
    mw(&port->fbu,  (u32)((d->page_phys + 0x400) >> 32));

    mw(&port->serr, 0xFFFFFFFFu);
    mw(&port->is,   0xFFFFFFFFu);
    mw(&port->ie,   0);                        /* polled driver — no port IRQs */

    port_start(port);

    /* The signature register only latches after the port engine is running and
     * the device has delivered its first D2H Register FIS (BSY/DRQ clear). */
    if (wait_bits(&port->tfd, AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ, 0, WAIT_LONG) != 0) {
        pr_debug("[AHCI]  port %u: device stuck busy, skipped\n", port_no);
        goto fail;
    }
    if (mr(&port->sig) != SATA_SIG_ATA) {
        pr_debug("[AHCI]  port %u: non-ATA signature %08x, skipped\n",
                 port_no, mr(&port->sig));
        goto fail;
    }

    if (ahci_identify(d) != 0) {
        pr_debug("[AHCI]  port %u: IDENTIFY failed, skipped\n", port_no);
        goto fail;
    }

    snprintf(d->bdev.name, sizeof(d->bdev.name), "sata%u", g_drive_count);
    d->bdev.sector_size = 512;
    d->bdev.ops         = &g_ahci_ops;
    d->bdev.driver_data = d;
    block_dev_register(&d->bdev);
    pr_debug("[AHCI]  registered /dev/%s\n", d->bdev.name);

    g_drive_count++;
    return;

fail:
    port_stop(port);
    pmm_free_page(d->page_phys);
    pmm_free_pages(d->bounce_phys, AHCI_BOUNCE_PAGES);
}

/* ── Controller bring-up ───────────────────────────────────────────────── */

static void ahci_controller_init(device_t *dev, pci_device_info_t *pci)
{
    pr_debug("[AHCI] controller %02x:%02x.%x (%04x:%04x)\n",
             pci->bus, pci->slot, pci->func, pci->vendor_id, pci->device_id);

    pci_enable_bus_mastering(dev);

    phys_addr_t abar_phys = pci_get_bar(dev, 5);
    if (!abar_phys) { pr_debug("[AHCI] no ABAR (BAR5)\n"); return; }

    phys_addr_t abar_pg  = ALIGN_DOWN(abar_phys, PAGE_SIZE);
    virt_addr_t abar_va  = (virt_addr_t)PHYS_TO_VIRT(abar_pg);
    for (u32 off = 0; off < 0x2000; off += PAGE_SIZE)
        vmm_map(0, abar_va + off, abar_pg + off, VMM_MMIO);

    ahci_hba_t *hba = (ahci_hba_t *)(abar_va + (abar_phys - abar_pg));

    /* BIOS/OS handoff (AHCI 1.2+). Ask for ownership and wait for BIOS to
     * release it; give up after ~1 s and take it anyway. */
    if (mr(&hba->cap2) & AHCI_CAP2_BOH) {
        mw(&hba->bohc, mr(&hba->bohc) | AHCI_BOHC_OOS);
        wait_bits(&hba->bohc, AHCI_BOHC_BOS, 0, WAIT_SHORT);
        for (u32 i = 0; i < WAIT_SHORT && (mr(&hba->bohc) & AHCI_BOHC_BB); i++)
            cpu_pause();
    }

    /* Enable AHCI, reset the HBA, re-enable AHCI. */
    mw(&hba->ghc, mr(&hba->ghc) | AHCI_GHC_AE);
    mw(&hba->ghc, mr(&hba->ghc) | AHCI_GHC_HR);
    if (wait_bits(&hba->ghc, AHCI_GHC_HR, 0, WAIT_LONG) != 0) {
        pr_debug("[AHCI] HBA reset timed out\n");
        return;
    }
    mw(&hba->ghc, mr(&hba->ghc) | AHCI_GHC_AE);

    u32 pi = mr(&hba->pi);
    u32 nports = (mr(&hba->cap) & 0x1F) + 1;
    for (u32 i = 0; i < 32; i++) {
        if (!(pi & (1U << i))) continue;
        if (i >= nports && nports < 32) continue;
        ahci_port_bringup(&hba->ports[i], i);
    }
}

static void ahci_walk(device_t *dev)
{
    for (; dev; dev = dev->sibling) {
        pci_device_info_t *pci = pci_get_device_info(dev);
        if (pci && pci->class_code == PCI_CLASS_MASS_STORAGE && pci->subclass == 0x06)
            ahci_controller_init(dev, pci);
        ahci_walk(dev->children);
    }
}

/* Entry point (declared in block.h, called from kernel_main). */
void block_ahci_init(void)
{
    pr_debug("[AHCI] scanning PCI for class 0x01 subclass 0x06 controllers...\n");
    g_drive_count = 0;
    ahci_walk(device_tree_root());
    if (g_drive_count == 0)
        pr_debug("[AHCI] no SATA drives found\n");
}
