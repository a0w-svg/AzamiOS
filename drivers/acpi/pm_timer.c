/* ============================================================================
 * AzamiOS — ACPI Power Management Timer (pm_timer) Driver
 * File: drivers/acpi/pm_timer.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "pm_timer.h"
#include "../../fs/vfs.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/lib/string.h"

static u16  g_pm_timer_port = PM_TIMER_DEFAULT_PORT;
static bool g_pm_timer_ready = false;
static bool g_pm_timer_is_32bit = false;

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

u32 pm_timer_read(void)
{
    if (!g_pm_timer_ready) return 0;
    u32 val = inl(g_pm_timer_port);
    return g_pm_timer_is_32bit ? val : (val & 0x00FFFFFF);
}

u64 pm_timer_ticks_to_ns(u32 ticks)
{
    return ((u64)ticks * 1000000000ULL) / PM_TIMER_FREQ_HZ;
}

/* ── Character Device Operations for /dev/pm_timer ───────────────────────── */

static s64 pm_timer_dev_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || !g_pm_timer_ready) return 0;
    if (*offset > 0) return 0;

    u32 ticks = pm_timer_read();
    u64 ns = pm_timer_ticks_to_ns(ticks);

    if (len >= 4 && len < 16) {
        /* Binary readout */
        memcpy(buf, &ticks, 4);
        *offset += 4;
        return 4;
    }

    char str[128];
    int n = scnprintf(str, sizeof(str),
                      "ACPI PM_TMR: %u ticks (%u.%06u s, freq: 3.579545 MHz, port: 0x%04X)\n",
                      ticks, (u32)(ns / 1000000000ULL), (u32)((ns % 1000000000ULL) / 1000),
                      g_pm_timer_port);

    if (len > (size_t)n) len = (size_t)n;
    memcpy(buf, str, len);
    *offset += len;
    return (s64)len;
}

static file_operations_t g_pm_timer_fops = {
    .read  = pm_timer_dev_read,
    .write = NULL,
    .ioctl = NULL,
};

/* ── Platform Driver ─────────────────────────────────────────────────────── */

#include "acpi.h"

static int pm_timer_probe(platform_device_t *pdev)
{
    u16 port = PM_TIMER_DEFAULT_PORT;

    acpi_fadt_t *fadt = (acpi_fadt_t *)acpi_find_table("FACP");
    if (fadt && fadt->pm_tmr_blk != 0) {
        port = (u16)fadt->pm_tmr_blk;
        if (fadt->flags & (1 << 8)) {
            g_pm_timer_is_32bit = true;
        }
    } else {
        const platform_resource_t *res = platform_get_resource(pdev, PLATFORM_RES_IO, 0);
        if (res && res->start > 0) {
            port = (u16)res->start;
        }
    }

    /* Verify timer oscillation */
    u32 t1 = inl(port);
    for (volatile int i = 0; i < 5000; i++) cpu_pause();
    u32 t2 = inl(port);

    if (t1 == 0xFFFFFFFF && t2 == 0xFFFFFFFF) {
        pr_debug("[PM_TMR] No ACPI PM Timer detected at I/O port 0x%04X\n", port);
        return -ENODEV;
    }

    g_pm_timer_port = port;
    g_pm_timer_ready = true;

    /* Check if 32-bit (bit 24..31 changing) */
    if ((t1 & 0xFF000000) != 0 || (t2 & 0xFF000000) != 0) {
        g_pm_timer_is_32bit = true;
    }

    devfs_register_device("pm_timer", &g_pm_timer_fops, NULL);

    pr_debug("[PM_TMR] ACPI Power Management Timer active at I/O 0x%04X (3.579545 MHz, %s, /dev/pm_timer)\n",
             port, g_pm_timer_is_32bit ? "32-bit" : "24-bit");
    return 0;
}

static platform_driver_t g_pm_timer_driver = {
    .drv = {
        .name = "pm_timer",
    },
    .probe  = pm_timer_probe,
    .remove = NULL,
};

int pm_timer_init(void)
{
    return platform_driver_register(&g_pm_timer_driver);
}
