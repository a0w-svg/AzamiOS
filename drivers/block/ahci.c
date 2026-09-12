/* ============================================================================
 * AzamiOS — AHCI 1.x / SATA Block Device Driver
 * File: drivers/block/ahci.c
 *
 * A polled (no-IRQ) AHCI port driver. For every SATA drive found behind a
 * PCI class-0x01 subclass-0x06 controller it:
 *
 *   1. performs the BIOS/OS ownership handoff and an HBA reset,
 *   2. rebases each implemented port onto a private 4 KiB DMA page
 *      (command list + received-FIS area + up to 8 command tables — slot 0
 *      reserved for non-queued control commands, slots 1-7 for NCQ),
 *   3. issues ATA IDENTIFY DEVICE to learn the real capacity / model / NCQ
 *      and TRIM support, and
 *   4. registers a block_dev_t named "sata0", "sata1", … so the VFS and
 *      /dev/sataN see a correctly-sized disk.
 *
 * Design choices that keep this simple and robust:
 *   - Every hardware wait loop is bounded; a stuck controller yields -EIO
 *     instead of wedging the kernel with a spinlock held and IRQs off
 *     (the old in-tree implementation had three unbounded `while` loops).
 *   - All data transfer goes through a per-drive physically-contiguous
 *     bounce buffer, so a caller-supplied buffer never has to be DMA-safe,
 *     page-aligned, or even part of the HHDM linear map.
 *   - Non-queued control commands (IDENTIFY, FLUSH CACHE EXT, TRIM, and
 *     ordinary READ/WRITE DMA EXT on a drive/HBA that doesn't support NCQ)
 *     always use slot 0 and a 64 KiB bounce buffer, fully serialised by
 *     d->lock exactly as this driver has always worked.
 *   - READ/WRITE (see "NCQ" below) use slots 1-7 when both the HBA and the
 *     drive advertise NCQ support, letting *different threads* have real
 *     concurrent I/O outstanding against the same disk; a single call still
 *     issues its own chunks one at a time, since there is no async I/O API
 *     above this driver to hand multiple chunks to concurrently — the
 *     concurrency this buys is between callers, not within one call.
 *   - NCQ error handling is deliberately conservative: this driver has no
 *     READ LOG EXT support to learn *which* queued command actually failed
 *     (that is what a real NCQ-capable driver does), so any error aborts
 *     every command currently outstanding on that port via a full port
 *     reset, guarded by a per-drive "epoch" counter so a waiter can never
 *     mistake "the port was just reset out from under me" for "my command
 *     completed" — see ncq_issue_and_wait()'s comment. That worst case has
 *     not been exercised against a real failure (QEMU's AHCI/NCQ emulation
 *     behaves correctly in every test this driver has been run against), so
 *     treat it as defensively designed rather than proven.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>

#include "ahci.h"
#include "block.h"
#include "../misc/hpet.h"
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

/* NCQ: slot 0 is reserved for the non-queued control path above, so slots
 * 1..AHCI_NCQ_MAX_SLOTS-1 (7 of them) are what this driver actually queues
 * into — clamped further at bring-up to whatever the HBA's CAP.NCS reports,
 * since a controller can implement fewer than 8 slots per port. */
#define AHCI_NCQ_MAX_SLOTS        8
#define AHCI_NCQ_SLOT_BOUNCE_BYTES (32 * 1024)     /* 64 sectors per command per slot */

/* Bounded-wait iteration budgets (each iteration does one cpu_pause()). */
#define WAIT_SHORT          2000000u
#define WAIT_LONG           20000000u

typedef struct ahci_drive {
    ahci_port_t *port;          /* MMIO port register block                 */
    u64          page_phys;     /* private CLB/FIS/cmdtbl page              */
    void        *page_virt;
    u64          bounce_phys;   /* slot-0-only 64 KiB bounce buffer         */
    void        *bounce_virt;
    u32          port_no;
    spinlock_t   lock;          /* control path (slot 0); NCQ slot bitmap + SAct/CI issue */

    /* NCQ (slots 1..ncq_nslots inclusive). ncq_active stays false — and
     * everything below it stays zeroed/unused — unless both the HBA and
     * this drive advertised NCQ support at bring-up. */
    bool         ncq_active;
    u32          ncq_nslots;    /* highest usable slot number, <= AHCI_NCQ_MAX_SLOTS-1 */
    u32          ncq_free;      /* bitmap: bit i set = slot i currently free */
    _Atomic u32  epoch;         /* bumped on every NCQ port reset; see ncq_issue_and_wait() */
    u64          ncq_bounce_phys;
    void        *ncq_bounce_virt;

    bool         trim_supported; /* IDENTIFY word 169 bit 0 */

    block_dev_t  bdev;
} ahci_drive_t;

static ahci_drive_t g_drives[AHCI_MAX_DRIVES];
static u32          g_drive_count;     /* total live entries in g_drives[]  */
static u32          g_sata_count;      /* naming counter: sata0, sata1, …   */
static u32          g_optical_count;   /* naming counter: sr0, sr1, …       */

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

/* Bring a wedged port back to a usable state: stop the engine (which drops any
 * command the HBA still thinks is outstanding and clears its CI shadow), scrub
 * latched error/interrupt status, restart. Without this one transient error
 * leaves the port BSY forever and every subsequent I/O eats the full WAIT_LONG
 * timeout with the drive lock held. */
static void port_recover(ahci_drive_t *d)
{
    ahci_port_t *p = d->port;
    port_stop(p);
    mw(&p->serr, 0xFFFFFFFFu);
    mw(&p->is,   0xFFFFFFFFu);
    port_start(p);
    wait_bits(&p->tfd, AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ, 0, WAIT_SHORT);
}

static int port_run_slot0(ahci_drive_t *d)
{
    ahci_port_t *p = d->port;

    /* Clear any latched error/interrupt status. */
    mw(&p->serr, 0xFFFFFFFFu);
    mw(&p->is, 0xFFFFFFFFu);

    /* Device must not be BSY/DRQ before we hand it a new command. */
    if (wait_bits(&p->tfd, AHCI_PxTFD_BSY | AHCI_PxTFD_DRQ, 0, WAIT_LONG) != 0) {
        port_recover(d);
        return -(s64)EIO;
    }

    mw(&p->ci, 1u);

    for (u32 i = 0; i < WAIT_LONG; i++) {
        u32 ci = mr(&p->ci);
        u32 is = mr(&p->is);
        if (is & AHCI_PxIS_ERR_MASK) {
            mw(&p->is, is);
            mw(&p->serr, 0xFFFFFFFFu);
            port_recover(d);
            return -(s64)EIO;
        }
        if ((ci & 1u) == 0) return 0;
        cpu_pause();
    }
    port_recover(d);           /* timed out — don't leave the port wedged */
    return -(s64)EIO;
}

/* ── Build the slot-0 command (FIS + single-entry PRDT) ─────────────────── */

/* @bytes == 0 builds a non-data command (e.g. FLUSH CACHE EXT): no PRDT
 * entry at all, since dbc is a "0-based count", not a length, and there is
 * no such thing as a zero-based count of zero bytes. */
static void build_cmd(ahci_drive_t *d, u8 ata_cmd, u64 lba, u16 sectors,
                      u32 bytes, int write)
{
    ahci_cmd_header_t *hdr   = (ahci_cmd_header_t *)d->page_virt;
    ahci_cmd_table_t  *tbl   = (ahci_cmd_table_t *)((u8 *)d->page_virt + 0x500);

    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl   = sizeof(fis_reg_h2d_t) / sizeof(u32);   /* FIS length in DWORDs */
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = bytes ? 1 : 0;
    hdr->ctba  = (u32)(d->page_phys + 0x500);
    hdr->ctbau = (u32)((d->page_phys + 0x500) >> 32);

    memset(tbl, 0, sizeof(*tbl));
    if (bytes) {
        tbl->prdt_entry[0].dba  = (u32)d->bounce_phys;
        tbl->prdt_entry[0].dbau = (u32)(d->bounce_phys >> 32);
        tbl->prdt_entry[0].dbc  = (bytes - 1) & 0x3FFFFF;   /* 22-bit byte count, 0-based */
    }

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

/* ── TRIM (ATA DATA SET MANAGEMENT) ───────────────────────────────────────
 *
 * A write-direction, non-queued command on slot 0, same as FLUSH CACHE EXT:
 * the "data" is a table of LBA-range entries the host sends to the device
 * (8 bytes each: 48-bit starting LBA, 16-bit range length), not sectors
 * being read or written. This driver only ever sends one entry per command
 * — simpler than packing several into one 512-byte block, at the cost of
 * one extra command for a trim spanning more than 65535 sectors, which is
 * not a hot enough path to matter.
 * ------------------------------------------------------------------------- */

static void build_cmd_dsm_trim(ahci_drive_t *d, u64 lba, u16 range_len)
{
    ahci_cmd_header_t *hdr = (ahci_cmd_header_t *)d->page_virt;
    ahci_cmd_table_t  *tbl = (ahci_cmd_table_t *)((u8 *)d->page_virt + 0x500);

    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl   = sizeof(fis_reg_h2d_t) / sizeof(u32);
    hdr->w     = 1;                 /* host -> device: the LBA-range table */
    hdr->prdtl = 1;
    hdr->ctba  = (u32)(d->page_phys + 0x500);
    hdr->ctbau = (u32)((d->page_phys + 0x500) >> 32);

    memset(tbl, 0, sizeof(*tbl));
    tbl->prdt_entry[0].dba  = (u32)d->bounce_phys;
    tbl->prdt_entry[0].dbau = (u32)(d->bounce_phys >> 32);
    tbl->prdt_entry[0].dbc  = (512 - 1) & 0x3FFFFF;   /* one 512 B block of range entries */

    u8 *entry = (u8 *)d->bounce_virt;
    memset(entry, 0, 512);
    entry[0] = (u8)lba;         entry[1] = (u8)(lba >> 8);
    entry[2] = (u8)(lba >> 16); entry[3] = (u8)(lba >> 24);
    entry[4] = (u8)(lba >> 32); entry[5] = (u8)(lba >> 40);
    entry[6] = (u8)range_len;   entry[7] = (u8)(range_len >> 8);

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = ATA_CMD_DATA_SET_MANAGEMENT;
    fis->featurel = 0x01;           /* TRIM */
    fis->countl   = 1;              /* one 512 B block of LBA-range data */
    fis->counth   = 0;
}

static s64 ahci_trim(block_dev_t *bd, u64 lba, u32 count)
{
    ahci_drive_t *d = (ahci_drive_t *)bd->driver_data;
    if (!d) return -(s64)EINVAL;
    if (lba + count > bd->sector_count) return -(s64)EINVAL;
    if (!d->trim_supported) return 0;   /* advisory: nothing this drive can do with it */

    spinlock_lock(&d->lock);

    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > 0xFFFF) chunk = 0xFFFF;   /* range length is a 16-bit field; never 0 here */

        build_cmd_dsm_trim(d, lba + done, (u16)chunk);
        int rc = port_run_slot0(d);
        if (rc != 0) {
            spinlock_unlock(&d->lock);
            return rc;
        }
        done += chunk;
    }

    spinlock_unlock(&d->lock);
    return 0;
}

/* ── NCQ (READ/WRITE FPDMA QUEUED, slots 1..ncq_nslots) ───────────────────
 *
 * Everything above stays on slot 0 and the legacy READ/WRITE DMA EXT
 * commands — unconditionally, whether or not NCQ is active — so a drive or
 * HBA that doesn't support NCQ (ncq_active stays false) is not affected by
 * any of this at all. What follows only ever runs for read_sectors()/
 * write_sectors() once ahci_port_bringup() has confirmed both the HBA
 * (CAP.SNCQ) and the drive (IDENTIFY word 76 bit 8) support it.
 * ------------------------------------------------------------------------- */

static inline void *ncq_slot_bounce_virt(ahci_drive_t *d, u32 slot)
{
    return (u8 *)d->ncq_bounce_virt + (size_t)(slot - 1) * AHCI_NCQ_SLOT_BOUNCE_BYTES;
}

static inline u64 ncq_slot_bounce_phys(ahci_drive_t *d, u32 slot)
{
    return d->ncq_bounce_phys + (u64)(slot - 1) * AHCI_NCQ_SLOT_BOUNCE_BYTES;
}

/* Blocks until a slot frees up rather than failing outright — the block_dev
 * API is synchronous, so a caller has no way to be told "try again later"
 * and act on it; every other outstanding command on this drive is expected
 * to retire well within WAIT_LONG. */
static int ncq_slot_alloc(ahci_drive_t *d)
{
    for (u32 spins = 0; spins < WAIT_LONG; spins++) {
        spinlock_lock(&d->lock);
        if (d->ncq_free) {
            int slot = __builtin_ctz(d->ncq_free);
            d->ncq_free &= ~(1u << slot);
            spinlock_unlock(&d->lock);
            return slot;
        }
        spinlock_unlock(&d->lock);
        cpu_pause();
    }
    return -1;
}

/* Only ever called on the success path of ncq_issue_and_wait() — an error
 * return from that function means a port reset already put every slot
 * (including this one) back in ncq_free itself, so freeing it again here
 * would be a double free. */
static void ncq_slot_free(ahci_drive_t *d, u32 slot)
{
    spinlock_lock(&d->lock);
    d->ncq_free |= (1u << slot);
    spinlock_unlock(&d->lock);
}

static void build_cmd_fpdma(ahci_drive_t *d, u32 slot, u8 ata_cmd, u64 lba,
                            u16 sectors, u32 bytes, int write)
{
    ahci_cmd_header_t *hdr  = (ahci_cmd_header_t *)d->page_virt + slot;
    u64 tbl_phys = d->page_phys + 0x500 + (u64)slot * sizeof(ahci_cmd_table_t);
    ahci_cmd_table_t *tbl   = (ahci_cmd_table_t *)((u8 *)d->page_virt + 0x500 +
                                                   (u32)slot * sizeof(ahci_cmd_table_t));
    u64  sbounce_phys = ncq_slot_bounce_phys(d, slot);

    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl   = sizeof(fis_reg_h2d_t) / sizeof(u32);
    hdr->w     = write ? 1 : 0;
    hdr->prdtl = bytes ? 1 : 0;
    hdr->ctba  = (u32)tbl_phys;
    hdr->ctbau = (u32)(tbl_phys >> 32);

    memset(tbl, 0, sizeof(*tbl));
    if (bytes) {
        tbl->prdt_entry[0].dba  = (u32)sbounce_phys;
        tbl->prdt_entry[0].dbau = (u32)(sbounce_phys >> 32);
        tbl->prdt_entry[0].dbc  = (bytes - 1) & 0x3FFFFF;
    }

    /* Same fis_reg_h2d_t layout as every other command here, but NCQ moves
     * two fields: sector count lives in Features (byte 3/11), and Count
     * (byte 12) carries the 5-bit TAG in its top bits instead of a count.
     * Getting this wrong is exactly the kind of bug that would only show up
     * as a wrong/corrupted transfer, which is why this is the one thing in
     * this driver checked against real hardware via a targeted self-test
     * (see AHCI-NCQ-SELFTEST) rather than trusted from spec-reading alone. */
    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = ata_cmd;              /* ATA_CMD_READ/WRITE_FPDMA_QUEUED */
    fis->featurel = (u8)sectors;
    fis->featureh = (u8)(sectors >> 8);
    fis->lba0 = (u8)lba;    fis->lba1 = (u8)(lba >> 8);  fis->lba2 = (u8)(lba >> 16);
    fis->lba3 = (u8)(lba >> 24); fis->lba4 = (u8)(lba >> 32); fis->lba5 = (u8)(lba >> 40);
    fis->device   = 1 << 6;                /* LBA mode; FUA left clear */
    fis->countl   = (u8)(slot << 3);       /* TAG in bits [7:3] */
    fis->counth   = 0;
}

/* Issues into @slot (already built via build_cmd_fpdma) and blocks for its
 * completion. Returns 0 on success, -1 on any failure — including one this
 * driver cannot attribute to a specific queued command (see the file
 * header's NCQ section), in which case every command outstanding on this
 * port was just aborted by a full reset, not only this one. */
static int ncq_issue_and_wait(ahci_drive_t *d, u32 slot)
{
    ahci_port_t *p = d->port;

    spinlock_lock(&d->lock);
    u32 my_epoch = __atomic_load_n(&d->epoch, __ATOMIC_RELAXED);
    /* AHCI §5.5.1: set SAct for this slot, then CI, for this slot. Both are
     * accumulate-additional-bits registers, so two threads issuing into
     * different slots at once must not race a read-modify-write of the
     * same register against each other — hence the lock, even though nothing
     * else here needs it. */
    mw(&p->sact, mr(&p->sact) | (1u << slot));
    mw(&p->ci,   mr(&p->ci)   | (1u << slot));
    spinlock_unlock(&d->lock);

    for (u32 i = 0; i < WAIT_LONG; i++) {
        if (__atomic_load_n(&d->epoch, __ATOMIC_RELAXED) != my_epoch) {
            /* Some other command on this port failed and reset it while we
             * were outstanding. Our own SAct bit reads clear now too, but
             * that is the reset talking, not our command completing — never
             * treat it as success. */
            return -1;
        }

        u32 is = mr(&p->is);
        if (is & AHCI_PxIS_ERR_MASK) {
            spinlock_lock(&d->lock);
            mw(&p->is, is);
            mw(&p->serr, 0xFFFFFFFFu);
            __atomic_fetch_add(&d->epoch, 1, __ATOMIC_RELAXED);   /* wake every other waiter */
            port_recover(d);
            /* A full reset aborts every queued command on this port, so
             * every NCQ slot is free again, no matter who thought they
             * still owned one — those waiters find out via the epoch check
             * above and must not free their slot a second time. */
            d->ncq_free = (d->ncq_nslots >= 31) ? 0xFFFFFFFEu
                                                : (((1u << d->ncq_nslots) - 1u) << 1);
            spinlock_unlock(&d->lock);
            return -1;
        }

        if (!(mr(&p->sact) & (1u << slot))) return 0;   /* this slot's SAct bit cleared: done */
        cpu_pause();
    }

    /* Timed out without ever seeing an error bit: the same conservative
     * response, since a port that will not retire a queued command is just
     * as wedged as one reporting an error. */
    spinlock_lock(&d->lock);
    __atomic_fetch_add(&d->epoch, 1, __ATOMIC_RELAXED);
    port_recover(d);
    d->ncq_free = (d->ncq_nslots >= 31) ? 0xFFFFFFFEu
                                        : (((1u << d->ncq_nslots) - 1u) << 1);
    spinlock_unlock(&d->lock);
    return -1;
}

static s64 ahci_rw_ncq(ahci_drive_t *d, u64 lba, u32 count, void *buf, int write)
{
    u32 max_per_cmd = AHCI_NCQ_SLOT_BOUNCE_BYTES / 512;
    u32 done = 0;

    while (done < count) {
        u32 chunk = count - done;
        if (chunk > max_per_cmd) chunk = max_per_cmd;
        u32 bytes = chunk * 512;

        int slot = ncq_slot_alloc(d);
        if (slot < 0) return done ? (s64)done * 512 : -(s64)EIO;

        void *sbuf = ncq_slot_bounce_virt(d, (u32)slot);
        if (write) memcpy(sbuf, (u8 *)buf + (u64)done * 512, bytes);

        build_cmd_fpdma(d, (u32)slot,
                        write ? ATA_CMD_WRITE_FPDMA_QUEUED : ATA_CMD_READ_FPDMA_QUEUED,
                        lba + done, (u16)chunk, bytes, write);

        int rc = ncq_issue_and_wait(d, (u32)slot);
        if (rc != 0) {
            /* ncq_issue_and_wait() already put every slot back in ncq_free
             * on this path (see its comment) — this one included. */
            return done ? (s64)done * 512 : -(s64)EIO;
        }

        if (!write) memcpy((u8 *)buf + (u64)done * 512, sbuf, bytes);
        ncq_slot_free(d, (u32)slot);

        done += chunk;
    }
    return (s64)count * 512;   /* bytes, matching ahci_rw()'s convention */
}

/* ── block_ops: read / write ───────────────────────────────────────────── */

static s64 ahci_rw(ahci_drive_t *d, u64 lba, u32 count, void *buf, int write)
{
    if (!buf || count == 0) return -(s64)EINVAL;
    if (lba + count > d->bdev.sector_count) return -(s64)EINVAL;

    /* Plain lock, not _irqsave: this driver is polled end to end (ie=0 at
     * every port, set in ahci_port_bringup) — nothing ever touches d->lock
     * from interrupt context, so there is no reentrancy hazard to guard
     * against by disabling interrupts. Masking them across a wait loop
     * bounded at WAIT_LONG (20,000,000 iterations) used to stall this
     * core's timer tick and IPI delivery for the entire round trip on every
     * single read/write. See the same fix applied to the virtio drivers'
     * submit paths (drivers/video/virtio_gpu.c and siblings). */
    spinlock_lock(&d->lock);

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
            spinlock_unlock(&d->lock);
            return rc;
        }

        if (!write)
            memcpy((u8 *)buf + (u64)done * 512, d->bounce_virt, bytes);

        done += chunk;
    }

    spinlock_unlock(&d->lock);
    return (s64)count * 512;   /* widen before *512 — count*512 overflows u32 at 2^23 sectors */
}

static s64 ahci_read_sectors(block_dev_t *bd, u64 lba, u32 count, void *buf)
{
    ahci_drive_t *d = (ahci_drive_t *)bd->driver_data;
    if (!d || !buf || count == 0) return -(s64)EINVAL;
    if (lba + count > bd->sector_count) return -(s64)EINVAL;
    if (d->ncq_active) return ahci_rw_ncq(d, lba, count, buf, 0);
    return ahci_rw(d, lba, count, buf, 0);
}

static s64 ahci_write_sectors(block_dev_t *bd, u64 lba, u32 count, const void *buf)
{
    ahci_drive_t *d = (ahci_drive_t *)bd->driver_data;
    if (!d || !buf || count == 0) return -(s64)EINVAL;
    if (lba + count > bd->sector_count) return -(s64)EINVAL;
    if (d->ncq_active) return ahci_rw_ncq(d, lba, count, (void *)buf, 1);
    return ahci_rw(d, lba, count, (void *)buf, 1);
}

static s64 ahci_flush(block_dev_t *bd)
{
    ahci_drive_t *d = (ahci_drive_t *)bd->driver_data;
    if (!d) return -(s64)EINVAL;

    spinlock_lock(&d->lock);
    build_cmd(d, ATA_CMD_FLUSH_CACHE_EXT, 0, 0, 0, 0);
    int rc = port_run_slot0(d);
    spinlock_unlock(&d->lock);
    return rc;
}

static block_ops_t g_ahci_ops = {
    .read_sectors  = ahci_read_sectors,
    .write_sectors = ahci_write_sectors,
    .flush         = ahci_flush,
    .trim          = ahci_trim,
};

/* ── ATAPI (SATA_SIG_ATAPI): CD/DVD drives ────────────────────────────────
 *
 * A port that comes up with SATA_SIG_ATAPI instead of SATA_SIG_ATA is a CD/
 * DVD drive — every AHCI/SATA controller QEMU exposes has one by default —
 * and it used to be logged as "non-ATA signature ..., skipped" and left
 * completely unused. ATAPI wraps SCSI: the ATA command is always PACKET
 * (0xA0), and the actual command is a 12-byte SCSI CDB carried in the
 * command table's acmd[] field instead of the FIS. Same READ CAPACITY(10)/
 * READ(10) commands drivers/block/virtio_scsi.c already speaks, just
 * delivered over AHCI's DMA engine instead of a virtqueue.
 * ------------------------------------------------------------------------- */

static void build_cmd_atapi(ahci_drive_t *d, const u8 *cdb12, u32 bytes)
{
    ahci_cmd_header_t *hdr = (ahci_cmd_header_t *)d->page_virt;
    ahci_cmd_table_t  *tbl = (ahci_cmd_table_t *)((u8 *)d->page_virt + 0x500);

    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl   = sizeof(fis_reg_h2d_t) / sizeof(u32);
    hdr->a     = 1;                     /* ATAPI: acmd[] carries the CDB */
    hdr->w     = 0;                     /* every command this driver issues reads */
    hdr->prdtl = bytes ? 1 : 0;
    hdr->ctba  = (u32)(d->page_phys + 0x500);
    hdr->ctbau = (u32)((d->page_phys + 0x500) >> 32);

    memset(tbl, 0, sizeof(*tbl));
    memcpy(tbl->acmd, cdb12, 12);
    if (bytes) {
        tbl->prdt_entry[0].dba  = (u32)d->bounce_phys;
        tbl->prdt_entry[0].dbau = (u32)(d->bounce_phys >> 32);
        tbl->prdt_entry[0].dbc  = (bytes - 1) & 0x3FFFFF;
    }

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = ATA_CMD_PACKET;
    fis->featurel = 1;                  /* DMA, not PIO, transfer */
}

/* Retries blind rather than fetching sense data through a separate REQUEST
 * SENSE command: a freshly spun-up ATAPI device commonly fails its first
 * command or two with CHECK CONDITION / UNIT ATTENTION (power-on, media
 * change) that clears itself on retry — exactly the same real-world
 * condition drivers/block/virtio_scsi.c's vscsi_cmd() retries around, just
 * without decoding *why* here. Good enough to get past bring-up and read
 * media; not a full SCSI initiator (see the file header's ATAPI section for
 * what that would take). */
static int ahci_atapi_cmd(ahci_drive_t *d, const u8 *cdb12, u32 bytes)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        build_cmd_atapi(d, cdb12, bytes);
        if (port_run_slot0(d) == 0) return 0;
    }
    return -1;
}

static int ahci_atapi_test_unit_ready(ahci_drive_t *d)
{
    u8 cdb[12] = {0};
    cdb[0] = 0x00;                      /* TEST UNIT READY */
    return ahci_atapi_cmd(d, cdb, 0);
}

static int ahci_atapi_read_capacity(ahci_drive_t *d, u64 *out_sectors, u32 *out_block_size)
{
    u8 cdb[12] = {0};
    cdb[0] = 0x25;                      /* READ CAPACITY (10) */
    if (ahci_atapi_cmd(d, cdb, 8) != 0) return -1;

    const u8 *data = (const u8 *)d->bounce_virt;
    u32 last_lba    = ((u32)data[0] << 24) | ((u32)data[1] << 16) |
                       ((u32)data[2] << 8)  |  (u32)data[3];
    u32 block_size  = ((u32)data[4] << 24) | ((u32)data[5] << 16) |
                       ((u32)data[6] << 8)  |  (u32)data[7];

    *out_sectors   = (u64)last_lba + 1;
    *out_block_size = block_size ? block_size : 2048;   /* the usual optical sector size */
    return 0;
}

/* READ (10) addresses a 32-bit LBA and a 16-bit block count, chunked the
 * same way virtio_scsi's vscsi_transfer10() is — and, like every optical
 * drive, read-only: g_ahci_atapi_ops leaves write_sectors NULL, which the
 * generic block layer (block_fops_write in drivers/block/block.c) already
 * turns into a clean failure rather than a crash. */
#define ATAPI_MAX_BLOCKS_PER_CMD 65535u

static s64 ahci_atapi_read_sectors(block_dev_t *bd, u64 lba, u32 count, void *buf)
{
    ahci_drive_t *d = (ahci_drive_t *)bd->driver_data;
    if (!d || !buf || count == 0) return -(s64)EINVAL;
    if (lba + count > bd->sector_count) return -(s64)EINVAL;

    u32 block_size = bd->sector_size;
    u32 max_per_cmd = AHCI_BOUNCE_BYTES / block_size;
    if (max_per_cmd > ATAPI_MAX_BLOCKS_PER_CMD) max_per_cmd = ATAPI_MAX_BLOCKS_PER_CMD;
    if (max_per_cmd == 0) return -(s64)EIO;

    spinlock_lock(&d->lock);   /* see the note on ahci_rw()'s lock */

    u32 done = 0;
    while (done < count) {
        u32 chunk = count - done;
        if (chunk > max_per_cmd) chunk = max_per_cmd;
        u32 bytes = chunk * block_size;
        u32 clba  = (u32)(lba + done);

        u8 cdb[12] = {0};
        cdb[0] = 0x28;                              /* READ (10) */
        cdb[2] = (u8)(clba >> 24); cdb[3] = (u8)(clba >> 16);
        cdb[4] = (u8)(clba >> 8);  cdb[5] = (u8)clba;
        cdb[7] = (u8)(chunk >> 8); cdb[8] = (u8)chunk;

        if (ahci_atapi_cmd(d, cdb, bytes) != 0) {
            spinlock_unlock(&d->lock);
            return -(s64)EIO;
        }
        memcpy((u8 *)buf + (u64)done * block_size, d->bounce_virt, bytes);
        done += chunk;
    }

    spinlock_unlock(&d->lock);
    return (s64)count * block_size;   /* bytes, matching ahci_rw()'s convention */
}

static block_ops_t g_ahci_atapi_ops = {
    .read_sectors  = ahci_atapi_read_sectors,
    /* .write_sectors and .flush stay NULL: read-only media. */
};

/* ── IDENTIFY DEVICE → capacity + model ─────────────────────────────────── */

static int ahci_identify(ahci_drive_t *d, bool *out_ncq_supported)
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

    /* Word 76 bit 8: NCQ (SATA capabilities — only valid on a SATA device,
     * which every ahci_identify() caller is by construction). Word 169
     * bit 0: DATA SET MANAGEMENT/TRIM supported. */
    if (out_ncq_supported) *out_ncq_supported = (id[76] & (1 << 8)) != 0;
    d->trim_supported = (id[169] & 1) != 0;

    /* Model string: words 27..46, byte-swapped ASCII. */
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = '\0';
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == '\0'); i--)
        model[i] = '\0';

    pr_debug("[AHCI]  port %u: '%s' — %llu sectors (%llu MiB), NCQ=%s TRIM=%s\n",
             d->port_no, model, (unsigned long long)sectors,
             (unsigned long long)(sectors / 2048),
             (out_ncq_supported && *out_ncq_supported) ? "yes" : "no",
             d->trim_supported ? "yes" : "no");
    return 0;
}

/* ── Per-port bring-up ─────────────────────────────────────────────────── */

static void ahci_port_bringup(ahci_port_t *port, u32 port_no, bool hba_ncq, u32 hba_ncs)
{
    if (g_drive_count >= AHCI_MAX_DRIVES) return;

    /* Wait for SATA PHY link (DET reaches 3). After an HBA reset a populated
     * port takes a few ms of COMRESET/COMWAKE negotiation; an *empty* port
     * reports DET==0 immediately, so bail fast once we've seen a stable 0 and
     * only spin the long budget while negotiation is actually in progress. */
    u32 ssts = 0;
    bool have_hpet = hpet_available();
    u64 t0 = have_hpet ? hpet_now_ns() : 0;
    for (u32 i = 0; i < WAIT_SHORT; i++) {
        ssts = mr(&port->ssts);
        u32 det = ssts & 0x0F;
        if (det == 3) break;                   /* device present + PHY up */
        /* det==1 means a device is there but the PHY hasn't finished
         * negotiating — keep the full budget for that. A genuinely empty slot
         * sits at det==0; give it a real ~15 ms grace (wall-clock when HPET is
         * up, iteration count otherwise) before writing it off. */
        if (det == 0) {
            if (have_hpet) { if (hpet_now_ns() - t0 > 15000000ULL) return; }
            else if (i > 200000) return;
        }
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
    u32 sig = mr(&port->sig);
    if (sig == SATA_SIG_ATAPI) {
        /* TEST UNIT READY first, both to clear the power-on UNIT ATTENTION
         * every ATAPI device starts with and to notice an empty tray before
         * spending a command on READ CAPACITY. An empty drive is not a
         * failure worth logging loudly — it just has nothing to register. */
        if (ahci_atapi_test_unit_ready(d) != 0) {
            pr_debug("[AHCI]  port %u: ATAPI drive not ready (no media?), skipped\n", port_no);
            goto fail;
        }
        u64 sectors = 0;
        u32 block_size = 2048;
        if (ahci_atapi_read_capacity(d, &sectors, &block_size) != 0 || sectors == 0) {
            pr_debug("[AHCI]  port %u: ATAPI READ CAPACITY failed, skipped\n", port_no);
            goto fail;
        }

        snprintf(d->bdev.name, sizeof(d->bdev.name), "sr%u", g_optical_count);
        d->bdev.sector_size  = block_size;
        d->bdev.sector_count = sectors;
        d->bdev.ops          = &g_ahci_atapi_ops;
        d->bdev.driver_data  = d;
        block_dev_register(&d->bdev);
        pr_debug("[AHCI]  port %u: ATAPI drive — %llu sectors x %u B (%llu MiB) -> /dev/%s\n",
                 port_no, (unsigned long long)sectors, block_size,
                 (unsigned long long)(sectors * block_size / (1024 * 1024)),
                 d->bdev.name);

        /* TEMP-SELFTEST: same idea as the ones in drivers/block/nvme.c and
         * drivers/block/virtio_scsi.c, but against real data instead of a
         * synthetic write/read-back pattern — this port very often *is* the
         * medium the kernel itself booted from (see block_ahci_init() in
         * kernel/main.c running before the boot ISO is mounted). LBA 16 of
         * an ISO9660 volume is its Primary Volume Descriptor, which starts
         * with a type byte (0x01) followed by the 5-byte identifier "CD001". */
        if (block_size <= 2048 && sectors > 16) {
            u8 pvd[2048];
            s64 r = ahci_atapi_read_sectors(&d->bdev, 16, 1, pvd);
            bool is_iso9660 = (r == (s64)block_size) &&
                              memcmp(&pvd[1], "CD001", 5) == 0;
            pr_debug("[AHCI-SELFTEST] port %u: LBA 16 read=%lld -> %s\n",
                     port_no, (long long)r, is_iso9660 ? "PASS (ISO9660 PVD found)" : "n/a (not ISO9660, or read failed)");
        }

        g_optical_count++;
        g_drive_count++;
        return;
    }
    if (sig == SATA_SIG_PM) {
        /* Port multiplier: a real one needs command-based switching (PMP
         * field routing per sub-drive, a soft reset to each PM port, and
         * reading the PM's own GSCR registers to enumerate what's behind
         * it) that this driver does not implement — and, as far as this
         * driver's testing goes, QEMU's AHCI model does not emulate a port
         * multiplier at all, so there is no way to exercise that code path
         * here even as a best-effort attempt. Reporting it distinctly from
         * a plain "non-ATA signature" at least tells a real user what is
         * actually behind that port instead of silently swallowing it. */
        pr_debug("[AHCI]  port %u: port multiplier detected — enumerating drives "
                 "behind it is not implemented, skipped\n", port_no);
        goto fail;
    }
    if (sig != SATA_SIG_ATA) {
        pr_debug("[AHCI]  port %u: non-ATA signature %08x, skipped\n", port_no, sig);
        goto fail;
    }

    bool drive_ncq = false;
    if (ahci_identify(d, &drive_ncq) != 0) {
        pr_debug("[AHCI]  port %u: IDENTIFY failed, skipped\n", port_no);
        goto fail;
    }

    /* NCQ bring-up: only if both sides agree. hba_ncs is the HBA's total
     * slot count (>=1); slot 0 stays reserved for the control path above,
     * so the usable NCQ range is 1..min(AHCI_NCQ_MAX_SLOTS-1, hba_ncs-1). A
     * failure here (out of memory) just leaves ncq_active false — the drive
     * still works, it only loses the concurrency benefit. */
    if (hba_ncq && drive_ncq) {
        u32 nslots = hba_ncs - 1;
        if (nslots > AHCI_NCQ_MAX_SLOTS - 1) nslots = AHCI_NCQ_MAX_SLOTS - 1;
        if (nslots >= 1) {
            d->ncq_bounce_phys = pmm_alloc_pages(nslots * AHCI_NCQ_SLOT_BOUNCE_BYTES / PAGE_SIZE);
            if (d->ncq_bounce_phys) {
                d->ncq_bounce_virt = PHYS_TO_VIRT(d->ncq_bounce_phys);
                d->ncq_nslots = nslots;
                d->ncq_free   = ((1u << nslots) - 1u) << 1;   /* bits 1..nslots */
                d->ncq_active = true;
                pr_debug("[AHCI]  port %u: NCQ active, %u queue slot%s\n",
                         port_no, nslots, nslots == 1 ? "" : "s");
            } else {
                pr_debug("[AHCI]  port %u: NCQ bounce allocation failed, falling back to non-queued I/O\n",
                         port_no);
            }
        }
    }

    snprintf(d->bdev.name, sizeof(d->bdev.name), "sata%u", g_sata_count);
    d->bdev.sector_size = 512;
    d->bdev.ops         = &g_ahci_ops;
    d->bdev.driver_data = d;
    block_dev_register(&d->bdev);
    pr_debug("[AHCI]  registered /dev/%s\n", d->bdev.name);

    /* TEMP-SELFTEST: FLUSH CACHE EXT is a non-data command (see build_cmd()'s
     * bytes==0 path) — round-tripping one for real here is what actually
     * proves that path works, since nothing else issues one during boot. */
    {
        s64 fr = ahci_flush(&d->bdev);
        pr_debug("[AHCI-SELFTEST] port %u: FLUSH CACHE EXT -> %s\n",
                 port_no, fr == 0 ? "PASS" : "FAIL");
    }

    /* TEMP-SELFTEST: TRIM a range and make sure the drive is still happy
     * answering ordinary reads/writes there afterward — TRIM leaves the
     * *contents* of a trimmed range unspecified, so this cannot check for
     * any particular data, only that the drive is not left wedged.
     *
     * This is not run against a disposable scratch image: whatever real
     * disk is behind this port (this driver has no idea whether it holds a
     * live filesystem — on a QEMU q35 machine, the default AHCI-attached
     * drive *is* the persistent /hdd volume) genuinely gets a TRIM issued
     * against it. Saving these sectors first and writing them back after is
     * what makes that safe: the disk is bit-for-bit what it was before this
     * ran, whether the drive actually discarded the range or left it alone. */
    if (d->trim_supported && d->bdev.sector_count > 208) {
        u8 saved[8 * 512];
        s64 sr = ahci_read_sectors(&d->bdev, 200, 8, saved);
        s64 tr = (sr > 0) ? ahci_trim(&d->bdev, 200, 8) : -(s64)EIO;
        u8 probe[512];
        s64 rr = (tr == 0) ? ahci_read_sectors(&d->bdev, 200, 1, probe) : -(s64)EIO;
        s64 restore = (sr > 0) ? ahci_write_sectors(&d->bdev, 200, 8, saved) : -(s64)EIO;
        pr_debug("[AHCI-SELFTEST] port %u: TRIM -> %s (original data restored: %s)\n",
                 port_no, (tr == 0 && rr > 0) ? "PASS" : "FAIL",
                 restore > 0 ? "yes" : "NO — sectors 200-207 may be altered");
    }

    /* TEMP-SELFTEST: this is what actually proves the NCQ FIS encoding in
     * build_cmd_fpdma() is right rather than just spec-read — two distinct
     * patterns written to two ranges, then read back via two READ FPDMA
     * QUEUED commands genuinely outstanding on two different slots *at the
     * same time* (both issued before either is waited on), each expected to
     * come back with its own range's pattern rather than the other's or a
     * torn mix of both.
     *
     * Same non-disposable-disk concern as the TRIM self-test above: this
     * writes real data to sectors 250-253 and 280-283 of whatever drive is
     * actually behind this port, so the original contents of both ranges
     * are saved first and written back at the end regardless of how the
     * test came out. */
    if (d->ncq_active && d->bdev.sector_count > 300) {
        u8 orig1[2048], orig2[2048];
        s64 sr1 = ahci_read_sectors(&d->bdev, 250, 4, orig1);
        s64 sr2 = ahci_read_sectors(&d->bdev, 280, 4, orig2);

        u8 wbuf1[2048], wbuf2[2048];
        for (u32 i = 0; i < sizeof(wbuf1); i++) wbuf1[i] = (u8)(i * 3 + 0x11);
        for (u32 i = 0; i < sizeof(wbuf2); i++) wbuf2[i] = (u8)(i * 5 + 0x22);
        s64 w1 = (sr1 > 0) ? ahci_write_sectors(&d->bdev, 250, 4, wbuf1) : -(s64)EIO;
        s64 w2 = (sr2 > 0) ? ahci_write_sectors(&d->bdev, 280, 4, wbuf2) : -(s64)EIO;

        int s1 = ncq_slot_alloc(d), s2 = (s1 >= 0) ? ncq_slot_alloc(d) : -1;
        bool overlapped = false;
        u8 rbuf1[2048], rbuf2[2048];
        s64 r1 = -1, r2 = -1;
        if (s1 >= 0 && s2 >= 0 && s1 != s2) {
            /* Build both commands and issue both before waiting on either —
             * this is the actual overlap: two commands genuinely in flight
             * on this port at once, not two that happen to use NCQ opcodes
             * one after another. */
            build_cmd_fpdma(d, (u32)s1, ATA_CMD_READ_FPDMA_QUEUED, 250, 4, 2048, 0);
            build_cmd_fpdma(d, (u32)s2, ATA_CMD_READ_FPDMA_QUEUED, 280, 4, 2048, 0);
            ahci_port_t *p = d->port;
            spinlock_lock(&d->lock);
            mw(&p->sact, mr(&p->sact) | (1u << s1) | (1u << s2));
            mw(&p->ci,   mr(&p->ci)   | (1u << s1) | (1u << s2));
            spinlock_unlock(&d->lock);
            overlapped = true;

            u32 my_epoch = __atomic_load_n(&d->epoch, __ATOMIC_RELAXED);
            int rc1 = -1, rc2 = -1;
            for (u32 i = 0; i < WAIT_LONG && (rc1 != 0 || rc2 != 0); i++) {
                if (__atomic_load_n(&d->epoch, __ATOMIC_RELAXED) != my_epoch) break;
                if (rc1 != 0 && !(mr(&p->sact) & (1u << s1))) {
                    memcpy(rbuf1, ncq_slot_bounce_virt(d, (u32)s1), 2048);
                    rc1 = 0;
                }
                if (rc2 != 0 && !(mr(&p->sact) & (1u << s2))) {
                    memcpy(rbuf2, ncq_slot_bounce_virt(d, (u32)s2), 2048);
                    rc2 = 0;
                }
                cpu_pause();
            }
            r1 = rc1 == 0 ? 2048 : -1;
            r2 = rc2 == 0 ? 2048 : -1;
            ncq_slot_free(d, (u32)s1);
            ncq_slot_free(d, (u32)s2);
        } else {
            if (s1 >= 0) ncq_slot_free(d, (u32)s1);
            if (s2 >= 0) ncq_slot_free(d, (u32)s2);
        }

        bool data_ok = overlapped && r1 == 2048 && r2 == 2048 &&
                       memcmp(rbuf1, wbuf1, sizeof(wbuf1)) == 0 &&
                       memcmp(rbuf2, wbuf2, sizeof(wbuf2)) == 0;

        s64 restore1 = (sr1 > 0) ? ahci_write_sectors(&d->bdev, 250, 4, orig1) : -(s64)EIO;
        s64 restore2 = (sr2 > 0) ? ahci_write_sectors(&d->bdev, 280, 4, orig2) : -(s64)EIO;

        pr_debug("[AHCI-NCQ-SELFTEST] port %u: w1=%lld w2=%lld overlapped=%d r1=%lld r2=%lld -> %s "
                 "(original data restored: %s)\n",
                 port_no, (long long)w1, (long long)w2, overlapped,
                 (long long)r1, (long long)r2,
                 (w1 > 0 && w2 > 0 && data_ok) ? "PASS" : "FAIL",
                 (restore1 > 0 && restore2 > 0) ? "yes"
                     : "NO — sectors 250-253/280-283 may be altered");
    }

    g_sata_count++;
    g_drive_count++;
    return;

fail:
    /* The DMA page we are about to free backs the command list and the received
     * FIS area. If the engine did not actually stop (misbehaving device —
     * port_stop returns non-zero), the HBA would keep DMAing FISes into a page
     * the PMM has handed to someone else. Force FRE/ST down and detach the
     * clb/fb pointers before releasing the page. */
    if (port_stop(port) != 0) {
        mw(&port->cmd, mr(&port->cmd) & ~(HBA_PxCMD_ST | HBA_PxCMD_FRE));
    }
    mw(&port->clb,  0);  mw(&port->clbu, 0);
    mw(&port->fb,   0);  mw(&port->fbu,  0);
    pmm_free_page(d->page_phys);
    pmm_free_pages(d->bounce_phys, AHCI_BOUNCE_PAGES);
}

/* ── Controller bring-up ───────────────────────────────────────────────── */

static int ahci_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    device_t *dev = dm->hal;
    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci) return -ENODEV;

    pr_debug("[AHCI] controller %02x:%02x.%x (%04x:%04x)\n",
             pci->bus, pci->slot, pci->func, pci->vendor_id, pci->device_id);

    pci_enable_bus_mastering(dev);

    phys_addr_t abar_phys = pci_get_bar(dev, 5);
    if (!abar_phys) { pr_debug("[AHCI] no ABAR (BAR5)\n"); return -ENODEV; }

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
        return -EIO;
    }
    mw(&hba->ghc, mr(&hba->ghc) | AHCI_GHC_AE);

    u32 cap = mr(&hba->cap);
    u32 pi = mr(&hba->pi);
    u32 nports = (cap & 0x1F) + 1;
    bool hba_ncq = (cap & (1U << 30)) != 0;      /* CAP.SNCQ */
    u32  hba_ncs = ((cap >> 8) & 0x1F) + 1;       /* CAP.NCS  */
    pr_debug("[AHCI] controller NCQ=%s, %u command slot%s\n",
             hba_ncq ? "yes" : "no", hba_ncs, hba_ncs == 1 ? "" : "s");
    for (u32 i = 0; i < 32; i++) {
        if (!(pi & (1U << i))) continue;
        if (i >= nports && nports < 32) continue;
        ahci_port_bringup(&hba->ports[i], i, hba_ncq, hba_ncs);
    }

    dm_set_drvdata(dm, (void *)hba);
    return 0;
}

static void ahci_remove(dm_device_t *dm)
{
    (void)dm;
    /* Ports/drives allocated by ahci_port_bringup() are torn down by
     * ahci_port_teardown() elsewhere; nothing controller-global to unwind
     * here beyond what a future HBA reset on re-probe already handles. */
}

/* Class 0x01 (mass storage), subclass 0x06 (AHCI/SATA) — any vendor, any
 * prog-if. One driver instance binds every matching controller the bus
 * finds, so multiple HBAs are still supported — the bus just calls probe()
 * once per match instead of this file walking the tree itself. */
static const pci_device_id_t ahci_pci_ids[] = {
    { PCI_DEVICE_CLASS(0x010600, 0xFFFF00) },
    { 0 }
};

static pci_driver_t ahci_pci_driver = {
    .drv      = { .name = "ahci" },
    .id_table = ahci_pci_ids,
    .probe    = ahci_probe,
    .remove   = ahci_remove,
};

/* Entry point (declared in block.h, called from kernel_main). Registers the
 * PCI driver; probe() binds to every matching AHCI controller the bus
 * already enumerated, so this is safe to call whether or not the host has
 * one. */
void block_ahci_init(void)
{
    g_drive_count   = 0;
    g_sata_count    = 0;
    g_optical_count = 0;
    pci_driver_register(&ahci_pci_driver);
    if (g_drive_count == 0)
        pr_debug("[AHCI] no SATA drives found\n");
}
