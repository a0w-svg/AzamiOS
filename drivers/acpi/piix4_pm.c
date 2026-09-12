/* ============================================================================
 * AzamiOS — Intel PIIX4 Power Management / ACPI Controller Driver
 * File: drivers/acpi/piix4_pm.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "piix4_pm.h"
#include "../../hal/pci.h"
#include "../base/pci_bus.h"
#include "../../fs/vfs.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/lib/string.h"

static u16  g_piix4_pm_base = 0;
static bool g_piix4_pm_ready = false;

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

u32 piix4_pm_read_timer(void)
{
    if (!g_piix4_pm_ready || g_piix4_pm_base == 0) return 0;
    return inl(g_piix4_pm_base + PIIX4_PM_TMR) & 0x00FFFFFF;
}

void piix4_pm_poweroff(void)
{
    pr_debug("[PIIX4_PM] Initiating ACPI S5 Soft Poweroff...\n");

    if (g_piix4_pm_ready && g_piix4_pm_base != 0) {
        /* PIIX4 PM1a_CNT S5 sleep state: SLP_TYP = 5, SLP_EN = 1 */
        outw(g_piix4_pm_base + PIIX4_PM_PM1_CNT, PIIX4_PM1_SLP_TYP_S5 | PIIX4_PM1_SLP_EN);
        for (volatile int i = 0; i < 50000; i++) cpu_pause();
        outw(g_piix4_pm_base + PIIX4_PM_PM1_CNT, 0x2000);
    }

    /* Fallback known ports */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    cpu_halt_loop();
}

/* ── Character Device Operations for /dev/acpi_pm ────────────────────────── */

static s64 piix4_pm_dev_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || !g_piix4_pm_ready) return 0;
    if (*offset > 0) return 0;

    u16 sts = inw(g_piix4_pm_base + PIIX4_PM_PM1_STS);
    u16 en  = inw(g_piix4_pm_base + PIIX4_PM_PM1_EN);
    u16 cnt = inw(g_piix4_pm_base + PIIX4_PM_PM1_CNT);
    u32 tmr = inl(g_piix4_pm_base + PIIX4_PM_TMR) & 0x00FFFFFF;
    u32 gpe = inl(g_piix4_pm_base + PIIX4_PM_GPE0_STS);

    char str[256];
    int n = scnprintf(str, sizeof(str),
                      "PIIX4 ACPI PM at I/O 0x%04X\n"
                      "PM1_STS: 0x%04X, PM1_EN: 0x%04X, PM1_CNT: 0x%04X\n"
                      "PM_TMR:  %u ticks (3.579545 MHz)\n"
                      "GPE0_STS: 0x%08X\n",
                      g_piix4_pm_base, sts, en, cnt, tmr, gpe);

    if (len > (size_t)n) len = (size_t)n;
    memcpy(buf, str, len);
    *offset += len;
    return (s64)len;
}

static s64 piix4_pm_dev_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (!g_piix4_pm_ready) return -ENODEV;

    switch (cmd) {
    case PM_IOC_GET_TIMER: {
        u32 tmr = piix4_pm_read_timer();
        if (arg) {
            if (copy_to_user((void *)arg, &tmr, sizeof(tmr)) != 0) return -EFAULT;
        }
        return (s64)tmr;
    }

    case PM_IOC_POWEROFF:
        piix4_pm_poweroff();
        return 0;

    case PM_IOC_GET_STATUS: {
        if (!arg) return -EFAULT;
        piix4_pm_status_t st;
        st.pm_base  = g_piix4_pm_base;
        st.pm1_sts  = inw(g_piix4_pm_base + PIIX4_PM_PM1_STS);
        st.pm1_en   = inw(g_piix4_pm_base + PIIX4_PM_PM1_EN);
        st.pm1_cnt  = inw(g_piix4_pm_base + PIIX4_PM_PM1_CNT);
        st.pm_tmr   = inl(g_piix4_pm_base + PIIX4_PM_TMR) & 0x00FFFFFF;
        st.gpe0_sts = inl(g_piix4_pm_base + PIIX4_PM_GPE0_STS);

        if (copy_to_user((void *)arg, &st, sizeof(st)) != 0) return -EFAULT;
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static file_operations_t g_piix4_pm_fops = {
    .read  = piix4_pm_dev_read,
    .write = NULL,
    .ioctl = piix4_pm_dev_ioctl,
};

/* ── PCI Probe / Driver Model ────────────────────────────────────────────── */

static int piix4_pm_pci_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -ENODEV;

    /* Read PMBASE at PCI config offset 0x40 */
    u32 pmbase_reg = pci_config_read32(info->bus, info->slot, info->func, PIIX4_PCI_PMBASE);
    u16 pm_base = (u16)(pmbase_reg & 0xFFC0);

    /* Fallback if unconfigured by BIOS/firmware */
    if (pm_base == 0) {
        pm_base = 0x0600;
        pci_config_write32(info->bus, info->slot, info->func, PIIX4_PCI_PMBASE, pm_base | 1);
    }

    /* Enable PM I/O Space Decoding in PMREGMISC (offset 0x80) */
    u8 pmregmisc = pci_config_read8(info->bus, info->slot, info->func, PIIX4_PCI_PMREGMISC);
    if (!(pmregmisc & PIIX4_PMIOSE_ENABLE)) {
        pci_config_write8(info->bus, info->slot, info->func, PIIX4_PCI_PMREGMISC,
                          pmregmisc | PIIX4_PMIOSE_ENABLE);
    }

    /* Enable I/O Space Access in PCI Command register */
    u16 pci_cmd = pci_config_read16(info->bus, info->slot, info->func, PCI_COMMAND);
    pci_config_write16(info->bus, info->slot, info->func, PCI_COMMAND, pci_cmd | PCI_CMD_IO_SPACE);

    g_piix4_pm_base  = pm_base;
    g_piix4_pm_ready = true;

    /* Register /dev/acpi_pm */
    devfs_register_device("acpi_pm", &g_piix4_pm_fops, NULL);

    pr_debug("[PIIX4_PM] Intel PIIX4 Power Management Controller at I/O 0x%04X (/dev/acpi_pm)\n",
             g_piix4_pm_base);
    return 0;
}

static const pci_device_id_t g_piix4_pm_pci_ids[] = {
    { PCI_DEVICE(0x8086, 0x7113) }, /* Intel 82371AB/EB/MB PIIX4 ACPI / Power Management */
    { 0 }
};

static pci_driver_t g_piix4_pm_pci_driver = {
    .drv = {
        .name = "piix4_pm",
    },
    .id_table = g_piix4_pm_pci_ids,
    .probe    = piix4_pm_pci_probe,
    .remove   = NULL,
};

int piix4_pm_init(void)
{
    return pci_driver_register(&g_piix4_pm_pci_driver);
}
