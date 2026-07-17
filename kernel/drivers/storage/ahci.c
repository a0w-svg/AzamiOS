/**
 * ahci.c — AzamiOS SATA AHCI Driver (HBA DMA, 32-bit phys, polling)
 *
 * Implements full AHCI port init, command list/FIS allocation, H2D FIS
 * construction, PRDT-based DMA, and polling command completion.
 */
#include "../include/ahci.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"
#include "../../klibc/include/string.h"
#include "../../filesystem/include/vfs.h"
#include "../../mem/include/paging.h"
#include "../../mem/include/pmm.h"

/* ── AHCI GHC register offsets ──────────────────────────────────────────── */
#define AHCI_GHC_CAP    0x00   /* Host Capabilities */
#define AHCI_GHC_GHC    0x04   /* Global Host Control */
#define AHCI_GHC_IS     0x08   /* Interrupt Status */
#define AHCI_GHC_PI     0x0C   /* Ports Implemented */
#define AHCI_GHC_VS     0x10   /* Version */

#define AHCI_GHC_AE     (1u << 31) /* AHCI Enable */
#define AHCI_GHC_HR     (1u << 0)  /* HBA Reset */

/* ── Port register offsets (relative to port base = ABAR + 0x100 + port*0x80) */
#define POFF_CLB   0x00   /* Command List Base Address (low) */
#define POFF_CLBU  0x04   /* Command List Base Address (high) */
#define POFF_FB    0x08   /* FIS Base Address (low) */
#define POFF_FBU   0x0C   /* FIS Base Address (high) */
#define POFF_IS    0x10   /* Interrupt Status */
#define POFF_IE    0x14   /* Interrupt Enable */
#define POFF_CMD   0x18   /* Command and Status */
#define POFF_TFD   0x20   /* Task File Data */
#define POFF_SIG   0x24   /* Signature */
#define POFF_SSTS  0x28   /* SATA Status */
#define POFF_SCTL  0x2C   /* SATA Control */
#define POFF_SERR  0x30   /* SATA Error */
#define POFF_SACT  0x34   /* SATA Active */
#define POFF_CI    0x38   /* Command Issue */

#define PORT_CMD_ST   (1u <<  0)  /* Start */
#define PORT_CMD_FRE  (1u <<  4)  /* FIS Receive Enable */
#define PORT_CMD_FR   (1u << 14)  /* FIS Receive Running */
#define PORT_CMD_CR   (1u << 15)  /* Command List Running */

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01
#define ATA_SR_DF   0x20

/* ── Structures ──────────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  cfl     : 5;   /* Command FIS length (dwords) */
    uint8_t  atapi   : 1;
    uint8_t  write   : 1;
    uint8_t  prefetch: 1;
    uint8_t  reset   : 1;
    uint8_t  bist    : 1;
    uint8_t  c       : 1;
    uint8_t  _res0   : 1;
    uint8_t  pmp     : 4;
    uint16_t prdtl;         /* Physical Region Descriptor Table Length */
    uint32_t prdbc;         /* PRDT Byte Count (filled by controller) */
    uint32_t ctba;          /* Command Table Base Address (low) */
    uint32_t ctbau;         /* Command Table Base Address (high) = 0 */
    uint32_t _res1[4];
} ahci_cmd_header_t;

typedef struct __attribute__((packed)) {
    uint32_t dba;    /* Data Base Address */
    uint32_t dbau;   /* Data Base Address (upper) = 0 */
    uint32_t _res;
    uint32_t dbc;    /* Byte count (0-indexed, bit 31 = interrupt on completion) */
} ahci_prdt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  cfis[64];      /* Command FIS (H2D Register FIS) */
    uint8_t  acmd[16];      /* ATAPI command (unused) */
    uint8_t  _res[48];
    ahci_prdt_entry_t prdt[1]; /* One PRDT entry for our DMA buffer */
} ahci_cmd_table_t;

/* H2D Register FIS layout (FIS type 0x27) */
typedef struct __attribute__((packed)) {
    uint8_t  fis_type;   /* 0x27 */
    uint8_t  pmport_c;   /* bit7 = C (command register update) */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;
    uint8_t  count_lo, count_hi;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  _res[4];
} h2d_fis_t;

/* ── State ───────────────────────────────────────────────────────────────── */

#define AHCI_MAX_PORTS 32
#define AHCI_POLL_TIMEOUT 500000

typedef struct {
    uintptr_t      bar;         /* ABAR physical/virtual (identity mapped) */
    uint8_t        active_port; /* first populated port index */
    uint64_t       sector_count;
    block_device_t bdev;
    bool           present;

    /* Per-port DMA structures (statically allocated, one per driver) */
    ahci_cmd_header_t *cmdlist;   /* 32 * sizeof(ahci_cmd_header_t) = 1024 B */
    uint8_t           *fis_buf;   /* 256 B received FIS area */
    ahci_cmd_table_t  *cmdtable;  /* one command table */
} ahci_state_t;

static ahci_state_t g_ahci;
static block_device_t ahci_dev;

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

static inline uint32_t ahci_rd(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void ahci_wr(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}
static inline uint32_t port_rd(uint8_t port, uint32_t off) {
    return ahci_rd(g_ahci.bar + 0x100 + (uintptr_t)port * 0x80, off);
}
static inline void port_wr(uint8_t port, uint32_t off, uint32_t val) {
    ahci_wr(g_ahci.bar + 0x100 + (uintptr_t)port * 0x80, off, val);
}

/* ── Port helpers ────────────────────────────────────────────────────────── */

static void port_stop(uint8_t port) {
    uint32_t cmd = port_rd(port, POFF_CMD);
    cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
    port_wr(port, POFF_CMD, cmd);
    for (int i = 0; i < 50000; i++) {
        uint32_t c = port_rd(port, POFF_CMD);
        if (!(c & (PORT_CMD_CR | PORT_CMD_FR))) break;
    }
}

static void port_start(uint8_t port) {
    uint32_t cmd = port_rd(port, POFF_CMD);
    cmd |= PORT_CMD_FRE | PORT_CMD_ST;
    port_wr(port, POFF_CMD, cmd);
}

static int port_init(uint8_t port) {
    port_stop(port);

    /* Allocate command list (1 KiB = 32 headers × 32 B) */
    void *cl = pmm_alloc_block();
    void *fb = pmm_alloc_block();
    void *ct = pmm_alloc_block();
    if (!cl || !fb || !ct) {
        kprintf("ahci: port %u: OOM for DMA buffers\n", port);
        if (cl) pmm_free_block(cl);
        if (fb) pmm_free_block(fb);
        if (ct) pmm_free_block(ct);
        return -1;
    }
    memset(cl, 0, 4096);
    memset(fb, 0, 4096);
    memset(ct, 0, 4096);

    g_ahci.cmdlist  = (ahci_cmd_header_t *)cl;
    g_ahci.fis_buf  = (uint8_t *)fb;
    g_ahci.cmdtable = (ahci_cmd_table_t *)ct;

    /* Point command header[0] at our command table */
    g_ahci.cmdlist[0].ctba  = (uint32_t)(uintptr_t)ct;
    g_ahci.cmdlist[0].ctbau = 0;

    /* Set port base registers */
    port_wr(port, POFF_CLB,  (uint32_t)(uintptr_t)cl);
    port_wr(port, POFF_CLBU, 0);
    port_wr(port, POFF_FB,   (uint32_t)(uintptr_t)fb);
    port_wr(port, POFF_FBU,  0);

    /* Clear error / interrupt bits */
    port_wr(port, POFF_SERR, 0xFFFFFFFFu);
    port_wr(port, POFF_IS,   0xFFFFFFFFu);

    port_start(port);
    return 0;
}

/* ── Command submission (polling) ────────────────────────────────────────── */

static int ahci_issue_cmd(uint8_t port, uint8_t cmd_byte,
                           uint64_t lba, uint16_t count,
                           uint32_t dma_phys, bool is_write) {
    /* Wait for port idle */
    for (int i = 0; i < AHCI_POLL_TIMEOUT; i++) {
        uint8_t tfd = (uint8_t)(port_rd(port, POFF_TFD) & 0xFF);
        if (!(tfd & (ATA_SR_BSY | ATA_SR_DRQ))) break;
        if (i == AHCI_POLL_TIMEOUT - 1) { kprintf("ahci: port busy timeout\n"); return -1; }
    }

    uint32_t nbytes = (uint32_t)count * 512;

    /* Fill command header */
    ahci_cmd_header_t *hdr = &g_ahci.cmdlist[0];
    memset(hdr, 0, sizeof(*hdr));
    hdr->cfl    = sizeof(h2d_fis_t) / 4;
    hdr->write  = is_write ? 1 : 0;
    hdr->prdtl  = 1;
    hdr->ctba   = (uint32_t)(uintptr_t)g_ahci.cmdtable;
    hdr->ctbau  = 0;

    /* Fill command table: H2D FIS */
    ahci_cmd_table_t *ct = g_ahci.cmdtable;
    memset(ct, 0, sizeof(*ct));
    h2d_fis_t *fis = (h2d_fis_t *)ct->cfis;
    fis->fis_type  = 0x27;
    fis->pmport_c  = 0x80;           /* C bit: update command register */
    fis->command   = cmd_byte;
    fis->device    = 0x40;           /* LBA mode */
    fis->lba0      = (uint8_t)(lba >>  0);
    fis->lba1      = (uint8_t)(lba >>  8);
    fis->lba2      = (uint8_t)(lba >> 16);
    fis->lba3      = (uint8_t)(lba >> 24);
    fis->lba4      = (uint8_t)(lba >> 32);
    fis->lba5      = (uint8_t)(lba >> 40);
    fis->count_lo  = (uint8_t)(count);
    fis->count_hi  = (uint8_t)(count >> 8);

    /* Fill PRDT entry */
    ct->prdt[0].dba  = dma_phys;
    ct->prdt[0].dbau = 0;
    ct->prdt[0].dbc  = nbytes - 1;   /* 0-indexed byte count */

    /* Clear port IS, issue command slot 0 */
    port_wr(port, POFF_IS, 0xFFFFFFFFu);
    port_wr(port, POFF_CI, 1u);

    /* Poll until CI[0] clears */
    for (int i = 0; i < AHCI_POLL_TIMEOUT; i++) {
        if (!(port_rd(port, POFF_CI) & 1u)) goto done;
        if (port_rd(port, POFF_IS) & (1u << 30)) { /* Task File Error */
            kprintf("ahci: TFE on port %u (TFD=%08x)\n", port, port_rd(port, POFF_TFD));
            return -1;
        }
    }
    kprintf("ahci: command timeout on port %u\n", port);
    return -1;

done:
    if (port_rd(port, POFF_IS) & (1u << 30)) {
        kprintf("ahci: TFE after completion on port %u\n", port);
        return -1;
    }
    return 0;
}

/* ── IDENTIFY ────────────────────────────────────────────────────────────── */

static int ahci_identify(uint8_t port) {
    void *buf = pmm_alloc_block();
    if (!buf) return -1;
    memset(buf, 0, 4096);

    if (ahci_issue_cmd(port, ATA_CMD_IDENTIFY, 0, 1,
                       (uint32_t)(uintptr_t)buf, false) != 0) {
        pmm_free_block(buf);
        return -1;
    }

    uint16_t *id = (uint16_t *)buf;
    /* Words 60-61: LBA28 sector count; words 100-103: LBA48 sector count */
    uint64_t lba28 = ((uint32_t)id[61] << 16) | id[60];
    uint64_t lba48 = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[101] << 16) |  (uint64_t)id[100];
    g_ahci.sector_count = lba48 ? lba48 : lba28;

    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = '\0';
    kprintf("ahci: port %u: %s  (%llu sectors, %llu MiB)\n",
            port, model,
            (unsigned long long)g_ahci.sector_count,
            (unsigned long long)(g_ahci.sector_count / 2048));

    pmm_free_block(buf);
    return 0;
}

/* ── block_device_t callbacks ────────────────────────────────────────────── */

static uint32_t ahci_block_read(block_device_t *dev,
                                 uint32_t lba, uint32_t count, void *buffer) {
    (void)dev;
    if (!g_ahci.present || !buffer || count == 0) return 0;

    uint8_t  *dst = (uint8_t *)buffer;
    uint32_t  done = 0;

    while (done < count) {
        uint32_t batch = count - done;
        if (batch > 8) batch = 8;               /* Max 8 sectors (4 KiB) per PMM block */
        uint32_t nbytes = batch * 512;

        void *dma = pmm_alloc_block();
        if (!dma) break;

        if (ahci_issue_cmd(g_ahci.active_port, ATA_CMD_READ_DMA_EXT,
                           (uint64_t)(lba + done), (uint16_t)batch,
                           (uint32_t)(uintptr_t)dma, false) != 0) {
            pmm_free_block(dma);
            break;
        }
        memcpy(dst + done * 512, dma, nbytes);
        pmm_free_block(dma);
        done += batch;
    }
    return done * 512;
}

static uint32_t ahci_block_write(block_device_t *dev,
                                  uint32_t lba, uint32_t count, void *buffer) {
    (void)dev;
    if (!g_ahci.present || !buffer || count == 0) return 0;

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t done = 0;

    while (done < count) {
        uint32_t batch = count - done;
        if (batch > 8) batch = 8;
        uint32_t nbytes = batch * 512;

        void *dma = pmm_alloc_block();
        if (!dma) break;

        memcpy(dma, src + done * 512, nbytes);
        if (ahci_issue_cmd(g_ahci.active_port, ATA_CMD_WRITE_DMA_EXT,
                           (uint64_t)(lba + done), (uint16_t)batch,
                           (uint32_t)(uintptr_t)dma, true) != 0) {
            pmm_free_block(dma);
            break;
        }
        pmm_free_block(dma);
        done += batch;
    }
    return done * 512;
}

/* ── Public init ─────────────────────────────────────────────────────────── */

void ahci_init(void) {
    memset(&g_ahci, 0, sizeof(g_ahci));

    pci_device_t *pdev = pci_find_class(0x01, 0x06, 0xFF);
    if (!pdev) {
        kprintf("ahci: no SATA AHCI controller found\n");
        return;
    }

    /* ABAR is BAR5 for AHCI */
    uint32_t abar_raw = pdev->bar[5];
    uintptr_t abar = (uintptr_t)(abar_raw & ~0xFu);
    if (!abar) {
        kprintf("ahci: invalid ABAR (BAR5=0)\n");
        return;
    }

    /* Map 64 KiB of MMIO space (identity) */
    for (uint32_t off = 0; off < 0x10000; off += 4096) {
        paging_map_page(abar + off, abar + off, 1, 1);
    }
    g_ahci.bar = abar;

    pci_enable_busmaster(pdev);
    kprintf("ahci: controller at PCI %u:%u.%u ABAR=0x%x IRQ=%u\n",
            pdev->bus, pdev->slot, pdev->func, (uint32_t)abar, pdev->irq);

    /* Enable AHCI mode */
    uint32_t ghc = ahci_rd(abar, AHCI_GHC_GHC);
    ahci_wr(abar, AHCI_GHC_GHC, ghc | AHCI_GHC_AE);

    uint32_t pi = ahci_rd(abar, AHCI_GHC_PI);

    for (uint8_t port = 0; port < AHCI_MAX_PORTS; port++) {
        if (!(pi & (1u << port))) continue;

        uint32_t ssts = port_rd(port, POFF_SSTS);
        uint8_t det = ssts & 0xF;
        uint8_t ipm = (ssts >> 8) & 0xF;
        if (det != 3 || ipm != 1) continue;  /* device present & active */

        uint32_t sig = port_rd(port, POFF_SIG);
        if (sig == 0xEB140101) {
            kprintf("ahci: port %u: ATAPI device, skipping\n", port);
            continue;
        }

        kprintf("ahci: port %u: SATA device detected (sig=0x%08x)\n", port, sig);
        if (port_init(port) != 0) continue;
        if (ahci_identify(port) != 0) continue;

        g_ahci.active_port = port;
        g_ahci.present = true;

        memset(&ahci_dev, 0, sizeof(ahci_dev));
        memcpy(ahci_dev.name, "sda", 4);
        ahci_dev.block_size = 512;
        ahci_dev.read  = ahci_block_read;
        ahci_dev.write = ahci_block_write;
        vfs_register_device(&ahci_dev);
        kprintf("ahci: /dev/sda registered\n");
        return;
    }
    kprintf("ahci: no active SATA drive found on any port\n");
}
