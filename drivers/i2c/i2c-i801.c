/* ============================================================================
 * AzamiOS — i2c-i801: Intel ICH / PIIX4 SMBus host controller
 * File: drivers/i2c/i2c-i801.c
 *
 * The SMBus block found in every Intel southbridge, and in QEMU's q35 (ICH9,
 * PCI 8086:2930) and i440fx (PIIX4, 8086:7113) machines.  It is a small I/O
 * register file that executes one SMBus protocol at a time: program the
 * address, command and data registers, write the protocol plus START into the
 * control register, then wait for the host-busy bit to drop.
 *
 * The controller cannot do arbitrary I2C transfers — it only knows the SMBus
 * protocols — so the adapter registers an smbus_xfer() and no master_xfer(),
 * which is exactly how the core distinguishes the two kinds of controller.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "i2c.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"

/* ── Register offsets from the SMBus I/O base ────────────────────────────── */
#define SMBHSTSTS   0x00
#define SMBHSTCNT   0x02
#define SMBHSTCMD   0x03
#define SMBHSTADD   0x04
#define SMBHSTDAT0  0x05
#define SMBHSTDAT1  0x06
#define SMBBLKDAT   0x07
#define SMBAUXCTL   0x0D

/* ── Status bits ─────────────────────────────────────────────────────────── */
#define STS_HOST_BUSY  0x01
#define STS_INTR       0x02
#define STS_DEV_ERR    0x04
#define STS_BUS_ERR    0x08
#define STS_FAILED     0x10
#define STS_SMBALERT   0x20
#define STS_INUSE      0x40
#define STS_BYTE_DONE  0x80
#define STS_ERROR_MASK (STS_DEV_ERR | STS_BUS_ERR | STS_FAILED)
#define STS_ALL        0xFF

/* ── Control bits ────────────────────────────────────────────────────────── */
#define CNT_INTREN     0x01
#define CNT_KILL       0x02
#define CNT_LAST_BYTE  0x20
#define CNT_START      0x40
#define CNT_PEC_EN     0x80

/* Protocol selector, bits 4:2 of the control register. */
#define XFER_QUICK      0x00
#define XFER_BYTE       0x04
#define XFER_BYTE_DATA  0x08
#define XFER_WORD_DATA  0x0C
#define XFER_PROC_CALL  0x10
#define XFER_BLOCK_DATA 0x14

/* PCI configuration: SMBus host enable lives outside the BARs. */
#define SMBHSTCFG      0x40
#define SMBHSTCFG_EN   0x01

#define I801_TIMEOUT_SPINS 500000

typedef struct i801_priv {
    u16 base;
    i2c_adapter_t adapter;
} i801_priv_t;

static inline void i801_out(i801_priv_t *p, u8 reg, u8 val) { outb((u16)(p->base + reg), val); }
static inline u8   i801_in(i801_priv_t *p, u8 reg)          { return inb((u16)(p->base + reg)); }

/* Wait for the controller to go idle, then clear any latched status. */
static int i801_wait_idle(i801_priv_t *p)
{
    for (u32 i = 0; i < I801_TIMEOUT_SPINS; i++) {
        if (!(i801_in(p, SMBHSTSTS) & STS_HOST_BUSY)) {
            i801_out(p, SMBHSTSTS, STS_ALL);
            return 0;
        }
        cpu_pause();
    }
    return -EBUSY;
}

/* Run the transaction already programmed into the registers. */
static int i801_run(i801_priv_t *p, u8 xfer)
{
    i801_out(p, SMBHSTCNT, (u8)(xfer | CNT_START));

    for (u32 i = 0; i < I801_TIMEOUT_SPINS; i++) {
        u8 sts = i801_in(p, SMBHSTSTS);
        if (sts & STS_HOST_BUSY) { cpu_pause(); continue; }

        i801_out(p, SMBHSTSTS, STS_ALL);
        if (sts & STS_ERROR_MASK) {
            /* DEV_ERR with nothing else set is a NAK: no chip at that
             * address, which is a normal outcome when probing a bus. */
            if (sts & STS_DEV_ERR) return -ENXIO;
            return -EIO;
        }
        if (sts & STS_INTR) return 0;
        return 0;
    }

    /* Abandon a wedged transaction so the next one can start. */
    i801_out(p, SMBHSTCNT, CNT_KILL);
    i801_out(p, SMBHSTSTS, STS_ALL);
    return -ETIMEDOUT;
}

static int i801_smbus_xfer(i2c_adapter_t *adap, u16 addr, u16 flags,
                           char read_write, u8 command, int size,
                           union i2c_smbus_data *data)
{
    (void)flags;
    i801_priv_t *p = (i801_priv_t *)adap->algo_data;

    if (addr > 0x7F) return -EINVAL;   /* the block has no 10-bit addressing */

    int ret = i801_wait_idle(p);
    if (ret) return ret;

    u8 xfer;
    u8 hstadd = (u8)((addr << 1) | (read_write == I2C_SMBUS_READ ? 1 : 0));

    switch (size) {
    case I2C_SMBUS_QUICK:
        xfer = XFER_QUICK;
        break;
    case I2C_SMBUS_BYTE:
        xfer = XFER_BYTE;
        /* On a write the payload rides in the command register. */
        if (read_write == I2C_SMBUS_WRITE) i801_out(p, SMBHSTCMD, command);
        break;
    case I2C_SMBUS_BYTE_DATA:
        xfer = XFER_BYTE_DATA;
        i801_out(p, SMBHSTCMD, command);
        if (read_write == I2C_SMBUS_WRITE) i801_out(p, SMBHSTDAT0, data->byte);
        break;
    case I2C_SMBUS_WORD_DATA:
        xfer = XFER_WORD_DATA;
        i801_out(p, SMBHSTCMD, command);
        if (read_write == I2C_SMBUS_WRITE) {
            i801_out(p, SMBHSTDAT0, (u8)(data->word & 0xFF));
            i801_out(p, SMBHSTDAT1, (u8)(data->word >> 8));
        }
        break;
    case I2C_SMBUS_PROC_CALL:
        xfer = XFER_PROC_CALL;
        i801_out(p, SMBHSTCMD, command);
        i801_out(p, SMBHSTDAT0, (u8)(data->word & 0xFF));
        i801_out(p, SMBHSTDAT1, (u8)(data->word >> 8));
        hstadd &= (u8)~1u;    /* a process call is always a write cycle */
        break;
    default:
        return -ENOTSUP;
    }

    i801_out(p, SMBHSTADD, hstadd);

    ret = i801_run(p, xfer);
    if (ret) return ret;

    if (read_write == I2C_SMBUS_READ || size == I2C_SMBUS_PROC_CALL) {
        switch (size) {
        case I2C_SMBUS_BYTE:
        case I2C_SMBUS_BYTE_DATA:
            data->byte = i801_in(p, SMBHSTDAT0);
            break;
        case I2C_SMBUS_WORD_DATA:
        case I2C_SMBUS_PROC_CALL:
            data->word = (u16)(i801_in(p, SMBHSTDAT0) | (i801_in(p, SMBHSTDAT1) << 8));
            break;
        default:
            break;
        }
    }
    return 0;
}

static u32 i801_functionality(i2c_adapter_t *adap)
{
    (void)adap;
    return I2C_FUNC_SMBUS_QUICK |
           I2C_FUNC_SMBUS_READ_BYTE       | I2C_FUNC_SMBUS_WRITE_BYTE |
           I2C_FUNC_SMBUS_READ_BYTE_DATA  | I2C_FUNC_SMBUS_WRITE_BYTE_DATA |
           I2C_FUNC_SMBUS_READ_WORD_DATA  | I2C_FUNC_SMBUS_WRITE_WORD_DATA |
           I2C_FUNC_SMBUS_PROC_CALL;
}

static const i2c_algorithm_t i801_algorithm = {
    .master_xfer   = NULL,          /* SMBus protocols only */
    .smbus_xfer    = i801_smbus_xfer,
    .functionality = i801_functionality,
};

/* ── PCI binding ─────────────────────────────────────────────────────────── */

static int i801_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;

    pci_device_info_t *info = to_pci_info(dm);
    if (!info) return -ENODEV;

    /* The SMBus I/O window is BAR4 on every member of this family. */
    u16 base = (u16)pci_get_bar(dm->hal, 4);
    if (base == 0) return -ENODEV;

    i801_priv_t *p = (i801_priv_t *)kzalloc(sizeof(i801_priv_t));
    if (!p) return -ENOMEM;
    p->base = base;

    /* The host controller is disabled out of reset on some parts. */
    u8 cfg = pci_config_read8(info->bus, info->slot, info->func, SMBHSTCFG);
    if (!(cfg & SMBHSTCFG_EN)) {
        pci_config_write8(info->bus, info->slot, info->func, SMBHSTCFG,
                          (u8)(cfg | SMBHSTCFG_EN));
    }
    /* Make sure I/O decoding is on, or every register read returns 0xFF. */
    u16 cmd = pci_config_read16(info->bus, info->slot, info->func, PCI_COMMAND);
    if (!(cmd & PCI_CMD_IO_SPACE)) {
        pci_config_write16(info->bus, info->slot, info->func, PCI_COMMAND,
                           (u16)(cmd | PCI_CMD_IO_SPACE));
    }

    /* Clear anything the firmware left latched. */
    i801_out(p, SMBHSTSTS, STS_ALL);
    i801_out(p, SMBAUXCTL, 0);

    snprintf(p->adapter.name, sizeof(p->adapter.name),
             "SMBus I801 adapter at %04x", base);
    p->adapter.algo      = &i801_algorithm;
    p->adapter.algo_data = p;
    p->adapter.dm        = dm;

    int ret = i2c_add_adapter(&p->adapter);
    if (ret != 0) {
        kfree(p);
        return ret;
    }

    dm_set_drvdata(dm, p);

    /* Probe the bus once so the log shows what is actually out there; a NAK
     * (-ENXIO) just means nothing answers at that address. */
    u32 found = 0;
    char list[128];
    int n = 0;
    for (u16 a = 0x03; a <= 0x77; a++) {
        union i2c_smbus_data d;
        memset(&d, 0, sizeof(d));
        if (i801_smbus_xfer(&p->adapter, a, 0, I2C_SMBUS_READ, 0,
                            I2C_SMBUS_BYTE, &d) == 0) {
            found++;
            if ((size_t)n < sizeof(list) - 8) n += scnprintf(list + n, sizeof(list) - (size_t)n, " 0x%02x", a);
        }
    }
    list[n] = '\0';

    pr_debug("[I801] SMBus at I/O 0x%04x — %u device%s on the bus:%s\n",
             base, found, found == 1 ? "" : "s", found ? list : " (none)");
    return 0;
}

static void i801_remove(dm_device_t *dm)
{
    i801_priv_t *p = (i801_priv_t *)dm_get_drvdata(dm);
    if (!p) return;
    i2c_del_adapter(&p->adapter);
    kfree(p);
}

static const pci_device_id_t i801_pci_ids[] = {
    { PCI_DEVICE(0x8086, 0x2930) },   /* ICH9 / QEMU q35     */
    { PCI_DEVICE(0x8086, 0x7113) },   /* PIIX4 / QEMU i440fx */
    { PCI_DEVICE(0x8086, 0x27DA) },   /* ICH7                */
    { PCI_DEVICE(0x8086, 0x283E) },   /* ICH8                */
    { PCI_DEVICE(0x8086, 0x3A30) },   /* ICH10               */
    { 0 }
};

static pci_driver_t i801_pci_driver = {
    .drv      = { .name = "i801_smbus" },
    .id_table = i801_pci_ids,
    .probe    = i801_probe,
    .remove   = i801_remove,
};

void i801_init(void)
{
    pci_driver_register(&i801_pci_driver);
}
